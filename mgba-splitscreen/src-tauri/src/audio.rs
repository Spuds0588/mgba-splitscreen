//! Minimal ALSA playback via runtime dlopen — no build-time system headers or
//! device dependencies. Loads `libasound.so.2` at runtime (present on any ALSA /
//! PipeWire-ALSA system) and plays the routed GBA mix at 32768 Hz stereo s16.
//! If ALSA can't load or open, audio silently no-ops: the app is video-first,
//! and the capture/routing machinery still works for future backends.
//!
//! The GBA core outputs 32768 Hz, 2-channel, interleaved s16 (see
//! `_GBACoreAudioSampleRate` and `mAudioBufferRead` in the core). The frame loop
//! drains each instance's ring once per tick and sends the routed/mixed result
//! here over a bounded channel; this thread writes it to the default PCM in
//! real time (a `try_send` in the frame loop means a slow device never stalls
//! emulation — audio simply drops frames, the same "drop to stay synced" policy
//! the video path uses).

use std::ffi::c_char;
use std::sync::mpsc::{sync_channel, Receiver, SyncSender};
use std::sync::OnceLock;

const SND_PCM_STREAM_PLAYBACK: i32 = 0;
const SND_PCM_FORMAT_S16_LE: i32 = 2;
const SND_PCM_ACCESS_RW_INTERLEAVED: i32 = 3;
const EAGAIN: i32 = 11;
const EPIPE: i32 = 32;
// Estimated one-way audio latency, microseconds. Big enough that brief
// emulation hiccups (turbo toggles, lockstep bursts) don't underrun.
const LATENCY_US: u32 = 80_000;

type SndPcm = *mut std::ffi::c_void;

/// Handle to the opened ALSA PCM device. Keeps the dlopen'd library alive.
struct AlsaSink {
    _lib: libloading::Library,
    pcm: SndPcm,
    writei: unsafe extern "C" fn(SndPcm, *const i16, u64) -> i64,
    recover: unsafe extern "C" fn(SndPcm, i32, i32) -> i32,
}

impl AlsaSink {
    unsafe fn open(rate: u32) -> Option<Self> {
        let lib = match libloading::Library::new("libasound.so.2") {
            Ok(l) => l,
            Err(e) => {
                eprintln!("[AUDIO] failed to dlopen libasound.so.2: {e}");
                return None;
            }
        };
        let snd_pcm_open: libloading::Symbol<
            unsafe extern "C" fn(*mut SndPcm, *const c_char, i32, i32) -> i32,
        > = match lib.get(b"snd_pcm_open") {
            Ok(s) => s,
            Err(e) => {
                eprintln!("[AUDIO] missing symbol snd_pcm_open: {e}");
                return None;
            }
        };
        let snd_pcm_set_params: libloading::Symbol<
            unsafe extern "C" fn(SndPcm, i32, i32, u32, u32, i32, u32) -> i32,
        > = match lib.get(b"snd_pcm_set_params") {
            Ok(s) => s,
            Err(e) => {
                eprintln!("[AUDIO] missing symbol snd_pcm_set_params: {e}");
                return None;
            }
        };
        let writei: libloading::Symbol<
            unsafe extern "C" fn(SndPcm, *const i16, u64) -> i64,
        > = match lib.get(b"snd_pcm_writei") {
            Ok(s) => s,
            Err(e) => {
                eprintln!("[AUDIO] missing symbol snd_pcm_writei: {e}");
                return None;
            }
        };
        let recover: libloading::Symbol<
            unsafe extern "C" fn(SndPcm, i32, i32) -> i32,
        > = match lib.get(b"snd_pcm_recover") {
            Ok(s) => s,
            Err(e) => {
                eprintln!("[AUDIO] missing symbol snd_pcm_recover: {e}");
                return None;
            }
        };
        let snd_pcm_close: libloading::Symbol<
            unsafe extern "C" fn(SndPcm) -> i32,
        > = match lib.get(b"snd_pcm_close") {
            Ok(s) => s,
            Err(e) => {
                eprintln!("[AUDIO] missing symbol snd_pcm_close: {e}");
                return None;
            }
        };
        let snd_strerror: libloading::Symbol<
            unsafe extern "C" fn(i32) -> *const c_char,
        > = match lib.get(b"snd_strerror") {
            Ok(s) => s,
            Err(e) => {
                eprintln!("[AUDIO] missing symbol snd_strerror: {e}");
                return None;
            }
        };

        let mut pcm: SndPcm = std::ptr::null_mut();
        let name = std::ffi::CString::new("default").ok()?;
        let r = snd_pcm_open(&mut pcm, name.as_ptr(), SND_PCM_STREAM_PLAYBACK, 0);
        if r < 0 {
            let msg = std::ffi::CStr::from_ptr(snd_strerror(r))
                .to_string_lossy()
                .into_owned();
            eprintln!("[AUDIO] snd_pcm_open failed ({r}): {msg}");
            return None;
        }
        let r = snd_pcm_set_params(
            pcm,
            SND_PCM_FORMAT_S16_LE,
            SND_PCM_ACCESS_RW_INTERLEAVED,
            2,
            rate,
            1, // soft_resample: let the plug layer resample rate -> device rate
            LATENCY_US,
        );
        if r < 0 {
            let msg = std::ffi::CStr::from_ptr(snd_strerror(r))
                .to_string_lossy()
                .into_owned();
            eprintln!("[AUDIO] snd_pcm_set_params failed ({r}): {msg}");
            let _ = snd_pcm_close(pcm);
            return None;
        }
        eprintln!("[AUDIO] ALSA sink open: default device at {rate} Hz stereo s16");
        // Keep the symbols alive by extracting the raw fn pointers; the Library
        // itself stays in the struct so the symbols remain valid.
        let writei = *writei;
        let recover = *recover;
        let _ = snd_strerror;
        Some(AlsaSink {
            _lib: lib,
            pcm,
            writei,
            recover,
        })
    }

    /// Write interleaved s16 stereo frames; recovers from underrun/overrun.
    fn play(&mut self, samples: &[i16]) {
        let frames = samples.len() / 2;
        if frames == 0 {
            return;
        }
        let mut off = 0usize;
        while off < frames {
            let n = unsafe {
                (self.writei)(
                    self.pcm,
                    samples.as_ptr().add(off * 2),
                    (frames - off) as u64,
                )
            };
            if n < 0 {
                let err = n as i32;
                if err == -EAGAIN {
                    std::thread::sleep(std::time::Duration::from_millis(5));
                    continue;
                }
                if err == -EPIPE {
                    // Underrun: recover (prepare) and retry the chunk.
                    let r = unsafe { (self.recover)(self.pcm, err, 1) };
                    if r < 0 {
                        return;
                    }
                    continue;
                }
                // Fatal device error: give up on this chunk silently.
                return;
            }
            off += n as usize;
        }
    }
}

impl Drop for AlsaSink {
    fn drop(&mut self) {
        unsafe {
            if let Ok(snd_pcm_close) = self._lib.get::<unsafe extern "C" fn(SndPcm) -> i32>(b"snd_pcm_close") {
                let _ = snd_pcm_close(self.pcm);
            }
        }
    }
}

type AudioChunk = (u32, Vec<i16>); // (sample rate, interleaved stereo s16)

static AUDIO_TX: OnceLock<SyncSender<AudioChunk>> = OnceLock::new();

/// Get (or lazily start) the audio output thread and return its sender.
/// Callers use `try_send` so a full/absent device never blocks emulation.
pub fn tx() -> &'static SyncSender<AudioChunk> {
    AUDIO_TX.get_or_init(|| {
        let (tx, rx) = sync_channel::<AudioChunk>(256);
        std::thread::Builder::new()
            .name("dualboy-audio".into())
            .spawn(move || audio_loop(rx))
            .expect("failed to spawn audio thread");
        tx
    })
}

fn audio_loop(rx: Receiver<AudioChunk>) {
    let mut sink: Option<AlsaSink> = None;
    let mut sink_rate: u32 = 0;
    let mut played_chunks: u64 = 0;
    let mut played_samples: u64 = 0;
    let mut last_report = std::time::Instant::now();
    loop {
        match rx.recv() {
            Ok((rate, samples)) => {
                played_chunks += 1;
                played_samples += samples.len() as u64;
                // Reopen the PCM when the source's rate changes (games switch
                // SOUNDBIAS resolution at runtime), so playback never drifts.
                if sink_rate != rate {
                    sink = None;
                    sink_rate = rate;
                    sink = unsafe { AlsaSink::open(rate) };
                }
                if let Some(sink) = sink.as_mut() {
                    sink.play(&samples);
                }
                if last_report.elapsed().as_secs() >= 5 {
                    eprintln!(
                        "[AUDIO] thread: {played_chunks} chunks, {played_samples} samples played, sink {} @ {sink_rate} Hz",
                        if sink.is_some() { "OPEN" } else { "FAILED" }
                    );
                    last_report = std::time::Instant::now();
                }
            }
            Err(_) => break,
        }
    }
}
