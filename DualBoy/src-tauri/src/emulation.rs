use std::io::Write;
use std::os::raw::{c_char, c_int};
use std::sync::atomic::{AtomicBool, AtomicU8, Ordering};
use std::sync::{Arc, Mutex, OnceLock};
use std::thread;
use std::time::{Duration, Instant};
use tokio::sync::broadcast;
use crate::audio;
use crate::gba::GbaInstance;
use crate::bindings;

/// Broadcast channel for status/overlay messages (one string per event). The frame
/// loop publishes per-second stats and stall warnings; mGBA WARN/ERROR/FATAL lines are
/// forwarded here too (see `overlay_log`). WS servers relay these to the frontend,
/// which renders them as an on-screen debug overlay.
static OVERLAY_TX: OnceLock<broadcast::Sender<String>> = OnceLock::new();

/// Broadcast channel for video frames (per player: one RGBA8888 buffer of
/// 240*160*4 bytes, all players concatenated per broadcast). Kept GLOBAL (OnceLock)
/// so WebSocket clients stay subscribed across `set_player_count` recreations of the
/// EmulationManager: the manager publishes to this channel and the WS relay
/// subscribes to it, so swapping in a new manager doesn't strand existing
/// connections on a dead per-manager channel.
static FRAME_TX: OnceLock<broadcast::Sender<Vec<u8>>> = OnceLock::new();

/// Broadcast channel for status/overlay lines. Global for the same reason as
/// FRAME_TX. `OVERLAY_TX` is a clone of this channel (set in `EmulationManager::new`),
/// so mGBA WARN+ lines and per-second stats share one stream that WS clients
/// subscribe to once and keep receiving across manager recreations.
static STATUS_TX: OnceLock<broadcast::Sender<String>> = OnceLock::new();

/// Broadcast channel for mixed audio chunks: (sample rate, interleaved stereo
/// s16). Global like FRAME_TX/STATUS_TX so WS relays keep their subscription
/// across manager recreations. The desktop app plays locally via ALSA (see
/// `audio::tx()`); the standalone web server relays this stream to browsers,
/// which play it through WebAudio.
static AUDIO_TX: OnceLock<broadcast::Sender<(u32, Vec<i16>)>> = OnceLock::new();

/// When true, the frame loop publishes audio ONLY to AUDIO_TX (browser playback)
/// and skips the local ALSA sink. The desktop app leaves this false (ALSA); the
/// standalone web server sets it true so sound plays in the browser instead of
/// doubling up on the server's speakers.
static AUDIO_TO_BROWSER: AtomicBool = AtomicBool::new(false);

/// Subscribe to the mixed audio stream (sample rate + interleaved stereo s16).
/// Used by the web server to relay audio to browsers for WebAudio playback.
pub fn subscribe_audio() -> broadcast::Receiver<(u32, Vec<i16>)> {
    AUDIO_TX
        .get_or_init(|| {
            let (tx, _) = broadcast::channel(32);
            tx
        })
        .subscribe()
}

/// Route audio output: `false` = local ALSA sink (desktop), `true` = broadcast
/// for browser WebAudio playback (standalone web server).
pub fn set_browser_audio(enabled: bool) {
    AUDIO_TO_BROWSER.store(enabled, Ordering::Relaxed);
    println!(
        "Audio output: {}",
        if enabled { "browser (WebAudio)" } else { "local (ALSA)" }
    );
    let _ = std::io::stdout().flush();
}

/// Wrap a video frame for the WS binary protocol: leading tag byte 0 = video.
pub fn encode_video(frame: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(frame.len() + 1);
    out.push(0u8);
    out.extend_from_slice(frame);
    out
}

/// Wrap an audio chunk for the WS binary protocol: tag byte 1 = audio, then a
/// u32 little-endian sample rate, then interleaved stereo s16 samples.
pub fn encode_audio(rate: u32, samples: &[i16]) -> Vec<u8> {
    let mut out = Vec::with_capacity(5 + samples.len() * 2);
    out.push(1u8);
    out.extend_from_slice(&rate.to_le_bytes());
    for s in samples {
        out.extend_from_slice(&s.to_le_bytes());
    }
    out
}

/// Audio routing: 1-4 = play that instance's full mix, 5 = mix all instances,
/// 0 = mute. Default 1 (Player 1). Lives outside any manager so it survives
/// manager recreation (player-count changes, quit game).
static AUDIO_SOURCE: AtomicU8 = AtomicU8::new(1);

pub fn set_audio_source(source: u8) {
    let s = source.clamp(0, 5);
    AUDIO_SOURCE.store(s, Ordering::Relaxed);
    let msg = match s {
        0 => "AUDIO muted".to_string(),
        5 => "AUDIO source: mix all players".to_string(),
        n => format!("AUDIO source: player {n}"),
    };
    println!("{msg}");
    let _ = std::io::stdout().flush();
    if let Some(tx) = OVERLAY_TX.get() {
        let _ = tx.send(msg);
    }
}

/// mGBA installs no default logger by itself; without one, `mLog()` prints every level
/// (including DEBUG BIOS SWI traces and lockstep chatter) to stdout on every call, which
/// floods the console and drags performance via synchronous I/O. Install the standard
/// logger once, restricted to WARN/ERROR/FATAL, and:
/// 1. Make C's stdout UNBUFFERED so mGBA's log lines hit the log file immediately even
///    if the process crashes (C stdout is fully buffered when redirected to a file, so
///    lines could be lost in a buffer on a hard crash).
/// 2. Override the log callback so WARN+ lines are also forwarded to the on-screen
///    debug overlay (OVERLAY_TX).
fn ensure_logger() {
    use std::sync::Once;
    static INIT: Once = Once::new();
    INIT.call_once(|| unsafe {
        bindings::setvbuf(bindings::stdout, std::ptr::null_mut(), bindings::_IONBF as c_int, 0);

        let logger: *mut bindings::mStandardLogger =
            Box::into_raw(Box::new(std::mem::zeroed::<bindings::mStandardLogger>()));
        bindings::mStandardLoggerInit(logger);
        (*logger).logToStdout = true;
        let filter = (*logger).d.filter;
        if !filter.is_null() {
            (*filter).defaultLevels = (bindings::mLogLevel_mLOG_WARN
                | bindings::mLogLevel_mLOG_ERROR
                | bindings::mLogLevel_mLOG_FATAL) as i32;
            // No per-category DEBUG here: enabling gba.sio DEBUG floods the log
            // AND the browser overlay with hundreds of lockstep lines per second,
            // which DOM-thrashes the frontend and makes inputs feel sluggish.
        }
        (*logger).d.log = Some(overlay_log);
        bindings::mLogSetDefaultLogger(&mut (*logger).d as *mut bindings::mLogger);
    });
}

/// mLogger callback: formats the message (mGBA passes a printf-style format + va_list),
/// prints it to stdout, and forwards it to the on-screen overlay channel.
unsafe extern "C" fn overlay_log(
    _logger: *mut bindings::mLogger,
    _category: c_int,
    _level: bindings::mLogLevel,
    format: *const c_char,
    args: *mut bindings::__va_list_tag,
) {
    if format.is_null() {
        return;
    }
    let mut buf = [0i8; 2048];
    let n = bindings::vsnprintf(
        buf.as_mut_ptr(),
        buf.len() as std::os::raw::c_ulong,
        format,
        args,
    );
    if n <= 0 {
        return;
    }
    let msg = std::ffi::CStr::from_ptr(buf.as_ptr())
        .to_string_lossy()
        .into_owned();
    println!("[mGBA] {msg}");
    if let Some(tx) = OVERLAY_TX.get() {
        let _ = tx.send(format!("[mGBA] {msg}"));
    }
}

/// mGBA's lockstep calls `user->sleep`/`wake` to block/resume a player's host thread
/// while it waits for the others to catch up (e.g. mid-transfer). Our emulation runs
/// every instance sequentially on one thread, so the callbacks just flip a per-player
/// flag: `GBASIOLockstepPlayerSleep` (in libmgba) also ends the current frame, and the
/// frame loop skips any player whose flag is set until it is woken. This is the
/// sequential-model equivalent of the primary's thread blocking until the secondary
/// delivers its data, and it keeps the coordinator's own `asleep` bookkeeping honest.
#[repr(C)]
struct LockstepUserCtx {
    base: bindings::mLockstepUser,
    /// Pointer into `EmulationManager::sleeping_flags` (Vec buffer is heap-stable).
    sleep_flag: *mut bool,
}

unsafe extern "C" fn lockstep_sleep(user: *mut bindings::mLockstepUser) {
    let ctx = user as *mut LockstepUserCtx;
    *(*ctx).sleep_flag = true;
}

unsafe extern "C" fn lockstep_wake(user: *mut bindings::mLockstepUser) {
    let ctx = user as *mut LockstepUserCtx;
    *(*ctx).sleep_flag = false;
}

/// Save-set file format: all instances' battery saves in one file.
/// Layout: b"DUALSAVE" | version:u32(1) | count:u32 | (size:u32, bytes)*
const SAVE_SET_MAGIC: &[u8; 8] = b"DUALSAVE";

fn serialize_save_set(saves: &[Vec<u8>]) -> Vec<u8> {
    let mut out = Vec::with_capacity(16 + saves.iter().map(|s| s.len() + 4).sum::<usize>());
    out.extend_from_slice(SAVE_SET_MAGIC);
    out.extend_from_slice(&1u32.to_le_bytes());
    out.extend_from_slice(&(saves.len() as u32).to_le_bytes());
    for s in saves {
        out.extend_from_slice(&(s.len() as u32).to_le_bytes());
        out.extend_from_slice(s);
    }
    out
}

fn deserialize_save_set(data: &[u8]) -> Result<Vec<Vec<u8>>, String> {
    if data.len() < 16 || &data[0..8] != SAVE_SET_MAGIC {
        return Err("Not a DualBoy save set".into());
    }
    let version = u32::from_le_bytes(data[8..12].try_into().unwrap());
    if version != 1 {
        return Err(format!("Unsupported save set version {version}"));
    }
    let count = u32::from_le_bytes(data[12..16].try_into().unwrap()) as usize;
    let mut saves = Vec::with_capacity(count);
    let mut off = 16;
    for _ in 0..count {
        if off + 4 > data.len() {
            return Err("Truncated save set".into());
        }
        let size = u32::from_le_bytes(data[off..off + 4].try_into().unwrap()) as usize;
        off += 4;
        if off + size > data.len() {
            return Err("Truncated save set".into());
        }
        saves.push(data[off..off + size].to_vec());
        off += size;
    }
    Ok(saves)
}

/// Save-state set format: all instances' in-memory save states in one blob.
/// Layout: b"DUALSTATE" | version:u32(1) | count:u32 | (size:u32, bytes)*
const STATE_SET_MAGIC: &[u8; 9] = b"DUALSTATE";

fn serialize_state_set(states: &[Vec<u8>]) -> Vec<u8> {
    let mut out =
        Vec::with_capacity(STATE_SET_MAGIC.len() + 8 + states.iter().map(|s| s.len() + 4).sum::<usize>());
    out.extend_from_slice(STATE_SET_MAGIC);
    out.extend_from_slice(&1u32.to_le_bytes());
    out.extend_from_slice(&(states.len() as u32).to_le_bytes());
    for s in states {
        out.extend_from_slice(&(s.len() as u32).to_le_bytes());
        out.extend_from_slice(s);
    }
    out
}

fn deserialize_state_set(data: &[u8]) -> Result<Vec<Vec<u8>>, String> {
    let hdr = STATE_SET_MAGIC.len() + 8;
    if data.len() < hdr || &data[0..STATE_SET_MAGIC.len()] != STATE_SET_MAGIC {
        return Err("Not a DualBoy save state set".into());
    }
    let version = u32::from_le_bytes(
        data[STATE_SET_MAGIC.len()..STATE_SET_MAGIC.len() + 4]
            .try_into()
            .unwrap(),
    );
    if version != 1 {
        return Err(format!("Unsupported save state set version {version}"));
    }
    let count = u32::from_le_bytes(
        data[STATE_SET_MAGIC.len() + 4..STATE_SET_MAGIC.len() + 8]
            .try_into()
            .unwrap(),
    ) as usize;
    let mut states = Vec::with_capacity(count);
    let mut off = hdr;
    for _ in 0..count {
        if off + 4 > data.len() {
            return Err("Truncated save state set".into());
        }
        let size = u32::from_le_bytes(data[off..off + 4].try_into().unwrap()) as usize;
        off += 4;
        if off + size > data.len() {
            return Err("Truncated save state set".into());
        }
        states.push(data[off..off + size].to_vec());
        off += size;
    }
    Ok(states)
}

/// Global quick-save-state slot: (player count, serialized state set). Lives outside
/// any manager so it survives `quit_game` / `set_player_count` recreations, like the
/// audio source. `load_state_set` checks the count matches before restoring.
static QUICK_STATE: OnceLock<Mutex<Option<(usize, Vec<u8>)>>> = OnceLock::new();

fn quick_state_slot() -> &'static Mutex<Option<(usize, Vec<u8>)>> {
    QUICK_STATE.get_or_init(|| Mutex::new(None))
}

/// Owns the lockstep coordinator and deinitializes it on drop.
/// Declared last in `EmulationManager` so the GBA instances are dropped first: their SIO
/// drivers call back into the coordinator during teardown, so it must outlive them.
struct LockstepCoordinator(Box<bindings::GBASIOLockstepCoordinator>);

impl Drop for LockstepCoordinator {
    fn drop(&mut self) {
        unsafe {
            bindings::GBASIOLockstepCoordinatorDeinit(&mut *self.0);
        }
    }
}

pub struct EmulationManager {
    pub instances: Vec<Arc<Mutex<GbaInstance>>>,
    /// Clone of the global FRAME_TX channel (see the statics above); the frame loop
    /// publishes here and the WS relay subscribes here. Kept as a field so tests and
    /// the standalone web server can subscribe without touching the statics.
    pub frame_sender: broadcast::Sender<Vec<u8>>,
    /// Status/overlay events (per-second stats, stall warnings, mGBA WARN+ lines).
    /// Clone of the global STATUS_TX channel, for the same reason as `frame_sender`.
    pub status_sender: broadcast::Sender<String>,
    /// Per-player "waiting for the link" flag, flipped by the lockstep sleep/wake
    /// callbacks (via raw pointers into this Vec's heap buffer). The frame loop
    /// skips a player while its flag is set.
    sleeping_flags: Arc<Mutex<Vec<bool>>>,
    /// Lockstep drivers, one per instance — EMPTY for solo (1-player) mode, where no
    /// cable is attached and games see "not connected".
    drivers: Vec<*mut bindings::GBASIOLockstepDriver>,
    // Kept alive for the manager's lifetime: the drivers hold raw pointers to these.
    _users: Vec<Box<LockstepUserCtx>>,
    // Some when linked play is active (count >= 2). Boxed so its address is stable:
    // the lockstep drivers hold a raw pointer to it.
    coordinator: Option<LockstepCoordinator>,
    /// Path of the ROM most recently loaded into every instance. `set_player_count`
    /// reads it to auto-reload the same game at the new count.
    loaded_rom: Mutex<Option<String>>,
    /// Set by `stop_and_join`; the frame-loop thread checks it each tick and exits.
    stop_flag: Arc<AtomicBool>,
    /// Turbo/fast-forward: when set, the frame loop skips its 60 Hz pacing sleep so
    /// emulation runs as fast as the host allows (lockstep still applies, so linked
    /// players stay in sync). The frontend toggles it (Tab key / Speed menu).
    turbo: Arc<AtomicBool>,
    /// Pause: when set, the frame loop stops advancing emulation across ALL instances
    /// at once (frozen mid-frame) but stays responsive to stop/unpause.
    paused: Arc<AtomicBool>,
    /// Join handle for the frame-loop thread (taken + joined by `stop_and_join`).
    thread: Mutex<Option<thread::JoinHandle<()>>>,
}

unsafe impl Send for EmulationManager {}
unsafe impl Sync for EmulationManager {}

impl EmulationManager {
    /// Create `count` GBA instances. 2-4 instances are linked over the virtual link
    /// cable (GBA supports up to `MAX_GBAS` = 4 players); 1 instance runs SOLO with
    /// no cable attached (authentic single-GBA behavior — link games show
    /// "not connected"). The count is clamped to [1, 4].
    pub fn new(count: usize) -> Self {
        let count = count.clamp(1, 4);
        // Broadcast channels are global so WebSocket clients stay subscribed when
        // `set_player_count` swaps in a new manager (see the channel statics above).
        let status_tx = STATUS_TX
            .get_or_init(|| {
                let (tx, _) = broadcast::channel(64);
                tx
            })
            .clone();
        let _ = OVERLAY_TX.set(status_tx.clone());
        ensure_logger();
        println!("Creating EmulationManager with {count} instances...");
        let tx = FRAME_TX
            .get_or_init(|| {
                let (tx, _) = broadcast::channel(10);
                tx
            })
            .clone();
        let _ = AUDIO_TX.get_or_init(|| {
            let (tx, _) = broadcast::channel(32);
            tx
        });

        let mut instances = Vec::with_capacity(count);
        for i in 0..count {
            instances.push(Arc::new(Mutex::new(GbaInstance::new((i + 1) as u8))));
        }

        let sleeping_flags = Arc::new(Mutex::new(vec![false; count]));

        // Lockstep coordinator + drivers exist only for linked play (2-4 players).
        let mut coordinator = None;
        let mut drivers = Vec::with_capacity(count.saturating_sub(1));
        let mut users = Vec::with_capacity(count.saturating_sub(1));
        if count >= 2 {
            let mut coord =
                Box::new(unsafe { std::mem::zeroed::<bindings::GBASIOLockstepCoordinator>() });
            unsafe {
                bindings::GBASIOLockstepCoordinatorInit(&mut *coord);
            }
            let flags_buf = sleeping_flags.lock().unwrap().as_mut_ptr();
            for i in 0..count {
                unsafe {
                    let driver = Box::into_raw(Box::new(std::mem::zeroed::<
                        bindings::GBASIOLockstepDriver,
                    >()));
                    let mut user = Box::new(std::mem::zeroed::<LockstepUserCtx>());
                    user.base.sleep = Some(lockstep_sleep);
                    user.base.wake = Some(lockstep_wake);
                    user.sleep_flag = flags_buf.add(i);

                    bindings::GBASIOLockstepDriverCreate(driver, &mut user.base);
                    bindings::GBASIOLockstepCoordinatorAttach(&mut *coord, driver);

                    drivers.push(driver);
                    users.push(user);
                }
            }
            coordinator = Some(LockstepCoordinator(coord));
        }

        EmulationManager {
            instances,
            frame_sender: tx,
            status_sender: status_tx,
            sleeping_flags,
            drivers,
            _users: users,
            coordinator,
            loaded_rom: Mutex::new(None),
            stop_flag: Arc::new(AtomicBool::new(false)),
            turbo: Arc::new(AtomicBool::new(false)),
            paused: Arc::new(AtomicBool::new(false)),
            thread: Mutex::new(None),
        }
    }

    pub fn player_count(&self) -> usize {
        self.instances.len()
    }

    /// Enable/disable turbo (fast-forward). Setting it while the loop is running
    /// takes effect on the next tick; emulation immediately stops pacing.
    pub fn set_turbo(&self, enabled: bool) {
        self.turbo.store(enabled, Ordering::Relaxed);
        let msg = if enabled {
            "TURBO ON - emulation fast-forwarding".to_string()
        } else {
            "TURBO OFF - back to 60 fps".to_string()
        };
        if let Some(tx) = OVERLAY_TX.get() {
            let _ = tx.send(msg.clone());
        }
        println!("{msg}");
        let _ = std::io::stdout().flush();
    }

    pub fn turbo_enabled(&self) -> bool {
        self.turbo.load(Ordering::Relaxed)
    }

    /// Pause/unpause emulation across all instances at once. While paused the
    /// frame loop holds the last frame and stops advancing game time; unpausing
    /// resumes exactly where it left off.
    pub fn set_paused(&self, paused: bool) {
        self.paused.store(paused, Ordering::Relaxed);
        let msg = if paused { "PAUSED (all players)" } else { "RESUMED" };
        if let Some(tx) = OVERLAY_TX.get() {
            let _ = tx.send(msg.to_string());
        }
        println!("{msg}");
        let _ = std::io::stdout().flush();
    }

    pub fn paused(&self) -> bool {
        self.paused.load(Ordering::Relaxed)
    }

    /// Path of the ROM currently loaded in every instance (None before first load).
    /// Used by `set_player_count` to auto-reload the same game at the new count.
    pub fn loaded_rom_path(&self) -> Option<String> {
        self.loaded_rom.lock().unwrap().clone()
    }

    /// Stop the frame-loop thread and wait for it to exit. Called by
    /// `set_player_count` before dropping a manager so an old loop never runs
    /// alongside the new one. Idempotent.
    pub fn stop_and_join(&self) {
        self.stop_flag.store(true, Ordering::Relaxed);
        if let Some(handle) = self.thread.lock().unwrap().take() {
            let _ = handle.join();
        }
    }

    /// Load the same ROM into every instance, attaching each instance's link-cable
    /// driver BEFORE the ROM boots so games detect the link at boot. Solo (1-player)
    /// loads attach no driver — there's no cable, which is how single-GBA play works.
    pub fn load_rom(&self, path: &str) -> Result<(), String> {
        let mut guards: Vec<_> = self
            .instances
            .iter()
            .map(|i| i.lock().map_err(|e| e.to_string()))
            .collect::<Result<_, _>>()?;

        if self.drivers.is_empty() {
            if !guards[0].load_rom(path, None) {
                return Err("Failed to load ROM".into());
            }
        } else {
            for (gba, driver) in guards.iter_mut().zip(&self.drivers) {
                if !gba.load_rom(path, Some(*driver)) {
                    return Err("Failed to load ROM in one or more instances".into());
                }
            }
        }
        drop(guards);
        *self.loaded_rom.lock().unwrap() = Some(path.to_string());
        if let Some(tx) = OVERLAY_TX.get() {
            let name = std::path::Path::new(path)
                .file_name()
                .and_then(|n| n.to_str())
                .unwrap_or(path);
            let _ = tx.send(format!("ROM loaded: {name}"));
        }
        Ok(())
    }

    pub fn set_keys(&self, player: u8, keys: u32) -> Result<(), String> {
        let idx = (player as usize)
            .checked_sub(1)
            .filter(|&i| i < self.instances.len())
            .ok_or_else(|| format!("Invalid player {player}"))?;
        self.instances[idx]
            .lock()
            .map_err(|e| e.to_string())?
            .set_keys(keys);
        Ok(())
    }

    /// Export the battery save of one instance (player is 1-based).
    pub fn export_save(&self, player: u8) -> Result<Vec<u8>, String> {
        let idx = (player as usize)
            .checked_sub(1)
            .filter(|&i| i < self.instances.len())
            .ok_or_else(|| format!("Invalid player {player}"))?;
        self.instances[idx]
            .lock()
            .map_err(|e| e.to_string())?
            .export_save()
            .ok_or_else(|| "No save data (game has no battery-backed save?)".into())
    }

    /// Import a battery save into one instance (player is 1-based).
    pub fn import_save(&self, player: u8, data: &[u8]) -> Result<(), String> {
        let idx = (player as usize)
            .checked_sub(1)
            .filter(|&i| i < self.instances.len())
            .ok_or_else(|| format!("Invalid player {player}"))?;
        if self.instances[idx]
            .lock()
            .map_err(|e| e.to_string())?
            .import_save(data)
        {
            Ok(())
        } else {
            Err("Failed to import save".into())
        }
    }

    /// Export all instances' saves as a single save-set blob.
    pub fn export_save_set(&self) -> Result<Vec<u8>, String> {
        let mut saves = Vec::with_capacity(self.player_count());
        for p in 1..=self.player_count() {
            saves.push(self.export_save(p as u8)?);
        }
        Ok(serialize_save_set(&saves))
    }

    /// Import all instances' saves from a save-set blob.
    pub fn import_save_set(&self, data: &[u8]) -> Result<(), String> {
        let saves = deserialize_save_set(data)?;
        if saves.len() != self.player_count() {
            return Err(format!(
                "Save set has {} saves but {} players are running",
                saves.len(),
                self.player_count()
            ));
        }
        for (p, save) in saves.iter().enumerate() {
            self.import_save((p + 1) as u8, save)?;
        }
        Ok(())
    }

    /// Capture a full save state of every instance as one blob. For linked play the
    /// whole set must be captured together so the round counters stay in lockstep.
    pub fn save_state_set(&self) -> Result<Vec<u8>, String> {
        let mut states = Vec::with_capacity(self.player_count());
        let mut guards: Vec<_> = self
            .instances
            .iter()
            .map(|i| i.lock().map_err(|e| e.to_string()))
            .collect::<Result<_, _>>()?;
        for g in guards.iter() {
            states.push(g.save_state().ok_or_else(|| "Failed to capture save state".to_string())?);
        }
        drop(guards);
        Ok(serialize_state_set(&states))
    }

    /// Restore every instance from a save-state-set blob captured by `save_state_set`.
    pub fn load_state_set(&self, data: &[u8]) -> Result<(), String> {
        let states = deserialize_state_set(data)?;
        if states.len() != self.player_count() {
            return Err(format!(
                "Save state set has {} states but {} players are running",
                states.len(),
                self.player_count()
            ));
        }
        let mut guards: Vec<_> = self
            .instances
            .iter()
            .map(|i| i.lock().map_err(|e| e.to_string()))
            .collect::<Result<_, _>>()?;
        for (g, state) in guards.iter_mut().zip(&states) {
            if !g.load_state(state) {
                return Err("Failed to restore save state".into());
            }
        }
        drop(guards);

        // The core state includes the SIO registers but NOT the external lockstep
        // coordinator's pending transfer/ack events, so restoring a linked game
        // mid-transfer would leave a player sleeping forever. Reset each driver to
        // abort any stale transfer and wake the players so the games re-handshake.
        // (Full coordinator-state capture is a follow-up; this makes loading a
        // state saved at a stable moment work in linked play.)
        for driver in &self.drivers {
            unsafe {
                let d = &mut (**driver).d;
                if let Some(reset) = d.reset {
                    reset(d as *mut _);
                }
            }
        }
        Ok(())
    }

    /// Quick-save: capture the set into the process-global slot (survives
    /// `quit_game`/player-count changes for the rest of the session).
    pub fn quick_save_state(&self) -> Result<(), String> {
        let blob = self.save_state_set()?;
        *quick_state_slot().lock().unwrap() = Some((self.player_count(), blob));
        Ok(())
    }

    /// Quick-load: restore the set from the global slot. Errors if the slot is
    /// empty or was captured at a different player count.
    pub fn quick_load_state(&self) -> Result<(), String> {
        let slot = quick_state_slot().lock().unwrap();
        let (count, blob) = slot.as_ref().ok_or("No quick save state yet")?;
        if *count != self.player_count() {
            return Err(format!(
                "Quick state was saved with {count} players but {} are running",
                self.player_count()
            ));
        }
        self.load_state_set(blob)
    }

    /// Is a given instance (0-based) currently waiting on the link (skipped by the
    /// frame loop)? Used by tests that drive frames directly.
    pub fn instance_sleeping(&self, i: usize) -> bool {
        self.sleeping_flags.lock().unwrap()[i]
    }

    /// Start the emulation loop. Emulation runs at a steady ~60 FPS (deadline-paced
    /// against a global 60 Hz clock, mGBA frame-limiter style) so the games keep
    /// correct speed and the lockstep link stays in sync. Every emulated frame is
    /// broadcast to the frontend (video default 60), so frames are rendered at speed
    /// instead of half of them being dropped by the wrapper. `video_fps` can only
    /// LOWER the send rate (every Nth frame) for bandwidth-constrained headless use;
    /// it never slows emulation. Slow consumers don't hold up emulation either — the
    /// broadcast channel drops frames for lagging receivers, which is the
    /// "drop frames to stay synced" contract.
    pub fn start(&self, video_fps: u32) {
        let instances = self.instances.clone();
        let sleeping = self.sleeping_flags.clone();
        let tx = self.frame_sender.clone();
        let status_tx = self.status_sender.clone();
        let n = instances.len();

        self.stop_flag.store(false, Ordering::Relaxed);
        let stop_flag = self.stop_flag.clone();
        let turbo = self.turbo.clone();
        let paused = self.paused.clone();
        let handle = thread::spawn(move || {
            // 60 Hz emulation pace. Each tick targets the next 16666 µs boundary from
            // a FIXED start, not "sleep 16.6 ms minus work" measured from the previous
            // tick's end: per-tick sleep overshoot (Linux ~1 ms granularity) just eats
            // into the next tick's budget instead of accumulating as drift, so the
            // average rate stays exactly 60 FPS (the old loop measured 57.5–59.5).
            let tick = Duration::from_micros(16666); // ~60 FPS
            let started = Instant::now();
            let mut next_deadline = started + tick;
            // video_fps=60 → every emulated frame is broadcast. Lower values drop
            // whole frames on the producer side (constrained headless use only).
            let video_every = (60 / video_fps.clamp(1, 60)).max(1);
            let mut tick_no = 0u32;

            // Per-second instrumentation: cumulative emu/video frame counters, snapshotted
            // once per second to derive per-instance FPS. A line is printed (and sent to
            // the overlay) every second so the log always has fresh data and frame drops
            // are visible when they happen.
            let started = Instant::now();
            let mut emu_frames = vec![0u64; n];
            let mut video_frames = 0u64;
            let mut last_stats = Instant::now();
            // Turbo video throttle: even flat-out emulation must not saturate the
            // WS relay with hundreds of frames/s — the display only shows ~60 Hz, so
            // ~100 video fps is plenty and keeps the socket healthy.
            let mut last_video = Instant::now();
            // Audio diagnostics (throttled): how many mix chunks we handed to the
            // audio thread, and how many samples total.
            let mut audio_played_chunks = 0u64;
            let mut audio_played_samples = 0u64;
            let mut prev_emu = emu_frames.clone();
            let mut prev_video = 0u64;
            let mut any_running;
            let mut stall_streak = 0u32;
            let mut was_stalled = false;

            // Per-player tear-free frame snapshots. mGBA's software renderer draws
            // scanlines into the output buffer incrementally, and a GBA game's vblank
            // handler typically clears it at the start of the next frame. Reading the
            // live buffer at an arbitrary point in the tick therefore catches it
            // mid-clear or mid-draw ("tearing"). The buffer is only guaranteed
            // complete for the instant between `finishFrame` (vcount 160) and the next
            // vblank handler running. We capture that instant: each `run_loop` step
            // returns right after the frame-end event sets `earlyExit`, and the video
            // frame counter has incremented by then but the ROM's vblank clear has not
            // yet run. Snapshotting on frame-counter increment freezes the complete
            // frame, and the broadcast always sends the last complete frame per player.
            const FRAME_BYTES: usize = 240 * 160 * 4;
            let mut snapshots: Vec<Vec<u8>> = vec![vec![0u8; FRAME_BYTES]; n];
            let mut last_fc: Vec<u32> = vec![u32::MAX; n];

            loop {
                // Stop check: `set_player_count` sets this flag and joins this
                // thread, so the old manager's loop never runs after the swap.
                if stop_flag.load(Ordering::Relaxed) {
                    break;
                }
                let is_paused = paused.load(Ordering::Relaxed);
                {
                    let mut guards: Vec<_> =
                        instances.iter().map(|i| i.lock().unwrap()).collect();

                    any_running = guards.iter().all(|g| g.is_running);
                    if any_running && !is_paused {
                        // Cooperative stepping: each tick advances every player by one
                        // video frame (~280896 cycles), switching between players
                        // whenever one sleeps on the lockstep link. This is the
                        // sequential equivalent of mGBA's threaded lockstep (a player's
                        // host thread blocks in user->sleep until another wakes it): a
                        // sleeping player is skipped, and the other player's work —
                        // delivering transfer data / acking — is what wakes it. Stepping
                        // one timing event at a time (instead of whole frames) lets a
                        // player pause mid-frame and resume exactly where it left off,
                        // so neither player's ROM frame gets split across ticks and
                        // both hold a steady ~60 fps.
                        const FRAME_CYCLES: i32 = 280_896; // GBA VIDEO_TOTAL_LENGTH
                        let frames_before: Vec<u32> =
                            guards.iter().map(|g| g.frame_counter()).collect();
                        let mut budgets = vec![FRAME_CYCLES; n];

                        let mut made_progress = true;
                        let mut steps = 0u32;
                        while made_progress && steps < 100_000 {
                            made_progress = false;
                            for i in 0..guards.len() {
                                if budgets[i] <= 0 {
                                    continue;
                                }
                                if sleeping.lock().unwrap()[i] {
                                    // Waiting on the link; skip until woken.
                                    continue;
                                }
                                made_progress = true;
                                let before = guards[i].current_time();
                                guards[i].run_loop();
                                let delta = guards[i]
                                    .current_time()
                                    .wrapping_sub(before)
                                    .max(1);
                                budgets[i] -= delta;
                                steps += 1;
                                // Frame complete? Snapshot the fully-drawn buffer
                                // before the ROM's vblank clear can scribble over it.
                                let fc = guards[i].frame_counter();
                                if fc != last_fc[i] {
                                    last_fc[i] = fc;
                                    snapshots[i] = guards[i].get_pixels_rgba();
                                }
                            }
                        }

                        for i in 0..guards.len() {
                            emu_frames[i] += guards[i]
                                .frame_counter()
                                .wrapping_sub(frames_before[i]) as u64;
                        }

                        tick_no = tick_no.wrapping_add(1);
                        let turbo_on = turbo.load(Ordering::Relaxed);
                        let every = if turbo_on { 1 } else { video_every };
                        let now = Instant::now();
                        if tick_no % every == 0
                            && (!turbo_on
                                || now.duration_since(last_video) >= Duration::from_millis(10))
                        {
                            // RGBA8888: 4 bytes/pixel so the frontend can putImageData
                            // directly with no per-pixel decode. Send the last complete
                            // frame snapshot per player (tear-free), never the live
                            // mid-frame buffer.
                            let mut combined = Vec::with_capacity(FRAME_BYTES * guards.len());
                            for snapshot in snapshots.iter() {
                                combined.extend_from_slice(snapshot);
                            }
                            let _ = tx.send(combined);
                            last_video = now;
                            video_frames += 1;
                        }

                        // ---- Audio: drain every instance once per tick, route
                        // per AUDIO_SOURCE. Skipped in turbo (the mix would play
                        // at real time while the game runs 3-8x, going stale).
                        let src = AUDIO_SOURCE.load(Ordering::Relaxed);
                        if src != 0 && !turbo_on {
                            let mut out: Vec<i16> = Vec::with_capacity(2048);
                            let mut rate = 0u32;
                            if (src as usize) <= guards.len() {
                                if let Some(g) = guards.get_mut((src as usize) - 1) {
                                    rate = g.sample_rate();
                                    out.extend_from_slice(g.drain_audio());
                                }
                            } else if src == 5 && guards.len() >= 2 {
                                // Mix all instances: saturating sum per sample. The
                                // mixed rate is the fastest of the sources.
                                let mut bufs: Vec<&[i16]> = Vec::with_capacity(guards.len());
                                for g in guards.iter_mut() {
                                    rate = rate.max(g.sample_rate());
                                    bufs.push(g.drain_audio());
                                }
                                let max = bufs.iter().map(|b| b.len()).max().unwrap_or(0);
                                out.reserve(max);
                                for i in 0..max {
                                    let mut sum: i32 = 0;
                                    for b in bufs.iter() {
                                        if i < b.len() {
                                            sum += b[i] as i32;
                                        }
                                    }
                                    out.push(sum.clamp(i16::MIN as i32, i16::MAX as i32) as i16);
                                }
                            }
                            if !out.is_empty() {
                                audio_played_chunks += 1;
                                audio_played_samples += out.len() as u64;
                                if audio_played_chunks % 300 == 0 {
                                    eprintln!(
                                        "[AUDIO] frame loop: src={src} sent {out_len} samples @ {rate} Hz (total {audio_played_chunks} chunks / {audio_played_samples} samples)",
                                        out_len = out.len()
                                    );
                                }
                                // Publish to the browser audio stream (the web
                                // server relays it to browsers; the desktop webview
                                // ignores it). Always published so there is a single
                                // source of truth for audio output.
                                if let Some(tx) = AUDIO_TX.get() {
                                    let _ = tx.send((rate, out.clone()));
                                }
                                // Local playback: ALSA on desktop. The standalone web
                                // server sets AUDIO_TO_BROWSER and skips this so sound
                                // doesn't double-play (server speakers + browser).
                                if !AUDIO_TO_BROWSER.load(Ordering::Relaxed) {
                                    let _ = audio::tx().try_send((rate, out));
                                }
                            }
                        }
                    }
                }

                // Wait for the next 60 Hz boundary. Small overruns (one slow frame)
                // just eat into the next tick's budget so the average rate stays 60;
                // only a large gap (host suspend/resume or a multi-second stall)
                // snaps the clock forward instead of fast-forwarding the game to
                // "catch up" after the resume.
                let now = Instant::now();
                if is_paused {
                    // Paused: freeze game time but keep the thread responsive to
                    // stop/unpause. Sleep short and skip the deadline bookkeeping so
                    // the 60 Hz clock snaps forward cleanly on resume instead of
                    // fast-forwarding the game to catch up.
                    thread::sleep(Duration::from_millis(16));
                } else if turbo.load(Ordering::Relaxed) {
                    // Turbo: no pacing — run as fast as the host allows. Lockstep
                    // sleeps/wakes still gate linked players inside the stepping loop,
                    // so they stay in sync at speed. A bare yield keeps the WS relay
                    // and frontend threads fed without throttling emulation, and an
                    // unloaded (no ROM) session idles briefly instead of hot-spinning.
                    thread::yield_now();
                    if !any_running {
                        thread::sleep(Duration::from_millis(8));
                    }
                } else {
                    if now < next_deadline {
                        thread::sleep(next_deadline - now);
                    } else if now - next_deadline > Duration::from_millis(250) {
                        next_deadline = now;
                    }
                    next_deadline += tick;
                }

                // ---- Per-second stats line (also streamed to the overlay) ----
                let now = Instant::now();
                if now.duration_since(last_stats) >= Duration::from_secs(1) {
                    let dt = now.duration_since(last_stats).as_secs_f64().max(0.001);
                    let per: Vec<String> = emu_frames
                        .iter()
                        .enumerate()
                        .map(|(i, &f)| {
                            format!("P{}=[{:.1}]", i + 1, (f - prev_emu[i]) as f64 / dt)
                        })
                        .collect();
                    let sleep: String = {
                        let s = sleeping.lock().unwrap();
                        s.iter().map(|&b| if b { "T" } else { "." }).collect()
                    };
                    let vfps = (video_frames - prev_video) as f64 / dt;
                    let paused_marker = if is_paused { " | PAUSED" } else { "" };
                    let line = format!(
                        "[t={:.0}s] emu {} fps | sleep:[{}] | video:{:.1} fps{}",
                        started.elapsed().as_secs_f64(),
                        per.join(" "),
                        sleep,
                        vfps,
                        paused_marker
                    );
                    println!("{line}");
                    let _ = std::io::stdout().flush();
                    let _ = status_tx.send(line);

                    // Stall detection: with a ROM loaded, any instance under 30 emu fps
                    // for 2+ consecutive seconds is a real slowdown (lockstep stall).
                    let min_emu = emu_frames
                        .iter()
                        .zip(&prev_emu)
                        .map(|(&f, &p)| (f - p) as f64 / dt)
                        .fold(f64::MAX, f64::min);
                    if any_running && !is_paused && min_emu < 30.0 {
                        stall_streak += 1;
                        if stall_streak >= 2 && !was_stalled {
                            was_stalled = true;
                            let msg = format!(
                                "WARN STALL: emulation under 30 fps for {stall_streak}s \
                                 (sleep:[{sleep}] min_emu={min_emu:.1} fps)"
                            );
                            println!("{msg}");
                            let _ = std::io::stdout().flush();
                            let _ = status_tx.send(msg);
                        }
                    } else if was_stalled && !is_paused {
                        was_stalled = false;
                        stall_streak = 0;
                        let msg = "OK recovered: emulation back above 30 fps".to_string();
                        println!("{msg}");
                        let _ = std::io::stdout().flush();
                        let _ = status_tx.send(msg);
                    }

                    prev_emu = emu_frames.clone();
                    prev_video = video_frames;
                    last_stats = now;
                }
            }
        });
        *self.thread.lock().unwrap() = Some(handle);
    }
}
