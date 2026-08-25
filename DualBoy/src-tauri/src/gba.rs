use std::ffi::{CString, c_char};
use crate::bindings;

// mAudioBuffer is opaque in the bindings; the ring-drain helpers below are real
// symbols in libmgba.a (src/util/audio-buffer.c) even though bindgen didn't
// emit them. mAudioBufferRead reads whole interleaved frames and returns the
// number of frames read; mAudioBufferAvailable reports how many frames are
// queued.
extern "C" {
    fn mAudioBufferRead(buffer: *mut bindings::mAudioBuffer, samples: *mut i16, count: usize) -> usize;
    fn mAudioBufferAvailable(buffer: *const bindings::mAudioBuffer) -> usize;
    /// Declared in include/mgba/core/core.h but missing from the MSVC-generated
    /// bindings; declare it explicitly so the build is portable.
    fn mCorePreloadFile(core: *mut bindings::mCore, path: *const c_char) -> bool;
    /// The mCore struct is allocated with malloc by `GBACoreCreate`; free it the
    /// same way when the core was never initialized (see Drop below).
    fn free(ptr: *mut core::ffi::c_void);
}

pub struct GbaInstance {
    pub id: u8,
    pub core: *mut bindings::mCore,
    pub is_running: bool,
    /// Whether `core->init` has run. mGBA's `_GBACoreDeinit` dereferences
    /// `core->cpu`/`core->board`, which are only allocated by `_GBACoreInit`;
    /// dropping a core that was created but never booted (a fresh manager swap
    /// with no ROM loaded) would NULL-deref in deinit. Only call deinit once the
    /// core has actually been initialized.
    initialized: bool,
    video_buffer: Vec<u32>,
    /// Per-instance audio drain destination (interleaved stereo s16).
    audio_buffer: Vec<i16>,
    /// Diagnostic tick counter for throttled audio logging.
    audio_dbg: u64,
}

impl GbaInstance {
    pub fn new(id: u8) -> Self {
        unsafe {
            let core = bindings::mCoreCreate(bindings::mPlatform_mPLATFORM_GBA);
            if core.is_null() {
                panic!("Failed to create mGBA core for instance {}", id);
            }
            
            GbaInstance {
                id,
                core,
                is_running: false,
                initialized: false,
                video_buffer: vec![0u32; 240 * 160],
                audio_buffer: Vec::new(),
                audio_dbg: 0,
            }
        }
    }

    /// Enable the core's audio ring (32768 Hz stereo s16, always written by the
    /// GBA audio mixer) and return how many interleaved stereo FRAMES queued
    /// since the last drain. Call once per emulation tick. Returns an empty
    /// slice when no ROM is loaded.
    pub fn drain_audio(&mut self) -> &[i16] {
        self.audio_buffer.clear();
        let mut avail = 0usize;
        let mut read = 0usize;
        unsafe {
            if !self.is_running {
                return &self.audio_buffer;
            }
            let get = (*self.core).getAudioBuffer;
            let Some(get) = get else {
                return &self.audio_buffer;
            };
            let buf = get(self.core);
            if buf.is_null() {
                return &self.audio_buffer;
            }
            avail = mAudioBufferAvailable(buf);
            if avail == 0 {
                return &self.audio_buffer;
            }
            let count = avail.min(4096);
            self.audio_buffer.resize(count * 2, 0);
            read = mAudioBufferRead(buf, self.audio_buffer.as_mut_ptr(), count);
            self.audio_buffer.truncate(read * 2);
        }
        // Throttled diagnostic: report ring fills + peak amplitude once per ~5s.
        self.audio_dbg += 1;
        if self.audio_dbg % 300 == 0 {
            let peak = self
                .audio_buffer
                .iter()
                .map(|s| s.unsigned_abs())
                .max()
                .unwrap_or(0);
            let nonzero = self.audio_buffer.iter().filter(|s| **s != 0).count();
            eprintln!(
                "[AUDIO] core ring: avail={avail} read={read} frames -> {} samples, peak={peak}, nonzero={nonzero}/{}",
                self.audio_buffer.len(),
                self.audio_buffer.len()
            );
        }
        &self.audio_buffer
    }

    /// Load a ROM and boot it. If `link_driver` is provided, it is attached to the
    /// link port BEFORE `reset`, so the game sees the link cable present at boot
    /// (mGBA's own QT multiplayer attaches the driver before the game runs).
    pub fn load_rom(
        &mut self,
        path: &str,
        link_driver: Option<*mut bindings::GBASIOLockstepDriver>,
    ) -> bool {
        // Use a relative path in the current working directory
        let temp_name = format!("temp_instance_{}.gba", self.id);
        println!("[GBA {}] Copying ROM to local file: {}...", self.id, temp_name);
        if let Err(e) = std::fs::copy(path, &temp_name) {
            println!("[GBA {}] Failed to copy ROM: {}", self.id, e);
            return false;
        }

        let c_path = CString::new(temp_name.as_str()).unwrap();
        
        unsafe {
            // mGBA init order (see src/gba/test/core.c): init -> initConfig -> load -> reset.
            // Loading the ROM before init dereferences core->board, which is NULL until
            // _GBACoreInit allocates it, so init MUST come first.
            println!("[GBA {}] Step 1: Initializing hardware...", self.id);
            if let Some(init_fn) = (*self.core).init {
                if !init_fn(self.core) {
                    println!("[GBA {}] Core init failed.", self.id);
                    let _ = std::fs::remove_file(temp_name);
                    return false;
                }
                self.initialized = true;
            }

            // Enable the core's audio ring so the mixer always writes samples;
            // the frame loop drains it once per tick for playback/routing.
            if let Some(set_size) = (*self.core).setAudioBufferSize {
                set_size(self.core, 2048);
            }

            println!("[GBA {}] Step 2: Loading config...", self.id);
            // mGBA frontends default opts.volume to 0x100 (100%). With no config
            // file, opts.volume stays 0 and _GBACoreLoadConfig sets
            // gba->audio.masterVolume = 0, so the core emits pure digital
            // silence (the mix runs, but _applyBias multiplies by 0). Set it
            // explicitly BEFORE loadConfig, which applies it.
            (*self.core).opts.volume = 0x100;
            bindings::mCoreInitConfig(self.core, std::ptr::null_mut());
            bindings::mCoreLoadConfig(self.core);

            println!("[GBA {}] Step 3: Setting video buffer...", self.id);
            if let Some(set_video_buffer_fn) = (*self.core).setVideoBuffer {
                set_video_buffer_fn(self.core, self.video_buffer.as_mut_ptr() as *mut _, 240);
            }

            println!("[GBA {}] Step 4: Preloading ROM file...", self.id);
            if !mCorePreloadFile(self.core, c_path.as_ptr()) {
                println!("[GBA {}] mCorePreloadFile failed.", self.id);
                let _ = std::fs::remove_file(temp_name);
                return false;
            }

            println!("[GBA {}] Step 4.5: Attaching link driver before boot...", self.id);
            if let Some(driver) = link_driver {
                self.set_sio_driver(driver);
            }

            println!("[GBA {}] Step 5: Resetting core...", self.id);
            if let Some(reset_fn) = (*self.core).reset {
                reset_fn(self.core);
            }

            self.is_running = true;
            println!("[GBA {}] GBA instance is now RUNNING.", self.id);
            
            // Cleanup temp file after load
            let _ = std::fs::remove_file(temp_name);
            true
        }
    }

    pub fn run_frame(&mut self) {
        unsafe {
            if let Some(run_frame_fn) = (*self.core).runFrame {
                run_frame_fn(self.core);
            }
        }
    }

    /// Advance the core by ONE scheduling step (until its next timing event, ~1232
    /// cycles = one GBA scanline). This is the fine-grained step the frame loop uses
    /// to cooperatively switch between players mid-frame: a player that sleeps during
    /// the step has its sleep flag set and is skipped until the other player wakes it.
    pub fn run_loop(&mut self) {
        unsafe {
            if let Some(run_loop_fn) = (*self.core).runLoop {
                run_loop_fn(self.core);
            }
        }
    }

    /// Current emulated cycle count (wrapping i32). The frame loop uses wrapping
    /// subtraction to measure per-step progress; that stays correct across the 2^31
    /// wrap as long as the measured span is well under 2^31 cycles (a frame is ~280k).
    pub fn current_time(&self) -> i32 {
        unsafe { bindings::mTimingCurrentTime((*self.core).timing) }
    }

    /// The core's current audio output rate (32768 Hz at SOUNDBIAS resolution 0,
    /// 65536 Hz at resolution 1). The GBA resamples internally to this rate, so
    /// the playback sink must match it or audio plays at the wrong speed.
    pub fn sample_rate(&self) -> u32 {
        unsafe {
            if let Some(f) = (*self.core).audioSampleRate {
                f(self.core)
            } else {
                32768
            }
        }
    }

    /// Video frame counter (increments once per rendered frame at vblank start).
    pub fn frame_counter(&self) -> u32 {
        unsafe {
            if let Some(frame_counter_fn) = (*self.core).frameCounter {
                frame_counter_fn(self.core)
            } else {
                0
            }
        }
    }

    pub fn set_sio_driver(&mut self, driver: *mut bindings::GBASIOLockstepDriver) {
        unsafe {
            println!("Attaching SIO driver to core...");
            if let Some(set_periph_fn) = (*self.core).setPeripheral {
                // mPERIPH_GBA_LINK_PORT is 0x1001
                set_periph_fn(self.core, 0x1001, driver as *mut _);
            }
        }
    }

    pub fn set_keys(&mut self, keys: u32) {
        // setKeys dereferences core->board, which is NULL until a ROM is loaded.
        if !self.is_running {
            return;
        }
        unsafe {
            if let Some(set_keys_fn) = (*self.core).setKeys {
                set_keys_fn(self.core, keys);
            }
        }
    }

    /// Pack the frame as RGBA8888 (4 bytes/pixel). The frontend feeds this buffer
    /// straight into `putImageData`, so sending RGBA means zero per-pixel decode work
    /// in JS (the old RGB565 path forced a per-pixel bit-swizzle on every frame in the
    /// browser, which was a bigger cost than the extra bandwidth on loopback/LAN).
    /// mGBA's `u32` buffer is already R,G,B in the low three bytes; OR the alpha in.
    pub fn get_pixels_rgba(&self) -> Vec<u8> {
        let mut pixels = Vec::with_capacity(240 * 160 * 4);
        for &pixel in &self.video_buffer {
            pixels.extend_from_slice(&(pixel | 0xFF00_0000).to_le_bytes());
        }
        pixels
    }

    /// Export the battery save (SRAM/flash) as raw `.sav` bytes, or None if the game
    /// has no save data. Compatible with mGBA's own .sav files.
    pub fn export_save(&self) -> Option<Vec<u8>> {
        if !self.is_running {
            return None;
        }
        unsafe {
            let clone_fn = (*self.core).savedataClone?;
            let mut sram: *mut std::ffi::c_void = std::ptr::null_mut();
            let size = clone_fn(self.core, &mut sram);
            if size == 0 || sram.is_null() {
                return None;
            }
            let data = std::slice::from_raw_parts(sram as *const u8, size).to_vec();
            bindings::free(sram);
            Some(data)
        }
    }

    /// Import a battery save from raw `.sav` bytes.
    pub fn import_save(&mut self, data: &[u8]) -> bool {
        if !self.is_running {
            return false;
        }
        unsafe {
            if let Some(restore_fn) = (*self.core).savedataRestore {
                return restore_fn(self.core, data.as_ptr() as *const _, data.len(), true);
            }
        }
        false
    }

    /// Capture a full save state (CPU/RAM/IO/audio/timers) as an in-memory
    /// byte blob. This is the core's native state format, not a battery save.
    pub fn save_state(&self) -> Option<Vec<u8>> {
        if !self.is_running {
            return None;
        }
        unsafe {
            let size_fn = (*self.core).stateSize?;
            let size = size_fn(self.core);
            if size == 0 {
                return None;
            }
            let save_fn = (*self.core).saveState?;
            let mut buf = vec![0u8; size];
            if !save_fn(self.core, buf.as_mut_ptr() as *mut _) {
                return None;
            }
            Some(buf)
        }
    }

    /// Restore a full save state previously captured with `save_state`.
    pub fn load_state(&mut self, data: &[u8]) -> bool {
        if !self.is_running {
            return false;
        }
        unsafe {
            if let Some(load_fn) = (*self.core).loadState {
                return load_fn(self.core, data.as_ptr() as *const _);
            }
        }
        false
    }
}

impl Drop for GbaInstance {
    fn drop(&mut self) {
        if !self.initialized {
            // Never booted: the core has no cpu/board allocated, and mGBA's
            // deinit would NULL-deref. Free the core struct itself instead.
            unsafe {
                free(self.core as *mut core::ffi::c_void);
            }
            return;
        }
        unsafe {
            if let Some(deinit_fn) = (*self.core).deinit {
                deinit_fn(self.core);
            }
        }
    }
}

unsafe impl Send for GbaInstance {}
unsafe impl Sync for GbaInstance {}
