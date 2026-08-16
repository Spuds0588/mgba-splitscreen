use std::ffi::CString;
use crate::bindings;

pub struct GbaInstance {
    pub id: u8,
    pub core: *mut bindings::mCore,
    pub is_running: bool,
    video_buffer: Vec<u32>,
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
                video_buffer: vec![0u32; 240 * 160],
            }
        }
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
            }

            println!("[GBA {}] Step 2: Loading config...", self.id);
            bindings::mCoreInitConfig(self.core, std::ptr::null_mut());
            bindings::mCoreLoadConfig(self.core);

            println!("[GBA {}] Step 3: Setting video buffer...", self.id);
            if let Some(set_video_buffer_fn) = (*self.core).setVideoBuffer {
                set_video_buffer_fn(self.core, self.video_buffer.as_mut_ptr() as *mut _, 240);
            }

            println!("[GBA {}] Step 4: Preloading ROM file...", self.id);
            if !bindings::mCorePreloadFile(self.core, c_path.as_ptr()) {
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
}

impl Drop for GbaInstance {
    fn drop(&mut self) {
        unsafe {
            if let Some(deinit_fn) = (*self.core).deinit {
                deinit_fn(self.core);
            }
        }
    }
}

unsafe impl Send for GbaInstance {}
unsafe impl Sync for GbaInstance {}
