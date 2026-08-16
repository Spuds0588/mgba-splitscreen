use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};
use tokio::sync::broadcast;
use crate::gba::GbaInstance;
use crate::bindings;

/// mGBA installs no default logger by itself; without one, `mLog()` prints every level
/// (including DEBUG BIOS SWI traces and lockstep chatter) to stdout on every call, which
/// floods the console and drags performance via synchronous I/O. Install the standard
/// logger once, restricted to WARN/ERROR/FATAL.
fn ensure_logger() {
    use std::sync::Once;
    static INIT: Once = Once::new();
    INIT.call_once(|| unsafe {
        let logger: *mut bindings::mStandardLogger =
            Box::into_raw(Box::new(std::mem::zeroed::<bindings::mStandardLogger>()));
        bindings::mStandardLoggerInit(logger);
        (*logger).logToStdout = true;
        let filter = (*logger).d.filter;
        if !filter.is_null() {
            (*filter).defaultLevels = (bindings::mLogLevel_mLOG_WARN
                | bindings::mLogLevel_mLOG_ERROR
                | bindings::mLogLevel_mLOG_FATAL) as i32;
        }
        bindings::mLogSetDefaultLogger(&mut (*logger).d as *mut bindings::mLogger);
    });
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
    pub frame_sender: broadcast::Sender<Vec<u8>>,
    /// Per-player "waiting for the link" flag, flipped by the lockstep sleep/wake
    /// callbacks (via raw pointers into this Vec's heap buffer). The frame loop
    /// skips a player while its flag is set.
    sleeping_flags: Arc<Mutex<Vec<bool>>>,
    drivers: Vec<*mut bindings::GBASIOLockstepDriver>,
    // Kept alive for the manager's lifetime: the drivers hold raw pointers to these.
    _users: Vec<Box<LockstepUserCtx>>,
    // Boxed so its address is stable: the lockstep drivers hold a raw pointer to it.
    coordinator: LockstepCoordinator,
}

unsafe impl Send for EmulationManager {}
unsafe impl Sync for EmulationManager {}

impl EmulationManager {
    /// Create `count` GBA instances linked over the virtual link cable. GBA supports up
    /// to `MAX_GBAS` (4) players; the count is clamped to [2, 4].
    pub fn new(count: usize) -> Self {
        let count = count.clamp(2, 4);
        ensure_logger();
        println!("Creating EmulationManager with {count} instances...");
        let (tx, _) = broadcast::channel(10);

        let mut coordinator =
            Box::new(unsafe { std::mem::zeroed::<bindings::GBASIOLockstepCoordinator>() });
        unsafe {
            bindings::GBASIOLockstepCoordinatorInit(&mut *coordinator);
        }

        let mut instances = Vec::with_capacity(count);
        for i in 0..count {
            instances.push(Arc::new(Mutex::new(GbaInstance::new((i + 1) as u8))));
        }

let sleeping_flags = Arc::new(Mutex::new(vec![false; count]));
        let flags_buf = sleeping_flags.lock().unwrap().as_mut_ptr();

        let mut drivers = Vec::with_capacity(count);
        let mut users = Vec::with_capacity(count);
        for i in 0..count {
            unsafe {
                let driver =
                    Box::into_raw(Box::new(std::mem::zeroed::<bindings::GBASIOLockstepDriver>()));
                let mut user = Box::new(std::mem::zeroed::<LockstepUserCtx>());
                user.base.sleep = Some(lockstep_sleep);
                user.base.wake = Some(lockstep_wake);
                user.sleep_flag = flags_buf.add(i);

                bindings::GBASIOLockstepDriverCreate(driver, &mut user.base);
                bindings::GBASIOLockstepCoordinatorAttach(&mut *coordinator, driver);

                drivers.push(driver);
                users.push(user);
            }
        }

        EmulationManager {
            instances,
            frame_sender: tx,
            sleeping_flags,
            drivers,
            _users: users,
            coordinator: LockstepCoordinator(coordinator),
        }
    }

    pub fn player_count(&self) -> usize {
        self.instances.len()
    }

    /// Load the same ROM into every instance, attaching each instance's link-cable
    /// driver BEFORE the ROM boots so games detect the link at boot.
    pub fn load_rom(&self, path: &str) -> Result<(), String> {
        let mut guards: Vec<_> = self
            .instances
            .iter()
            .map(|i| i.lock().map_err(|e| e.to_string()))
            .collect::<Result<_, _>>()?;

        for (gba, driver) in guards.iter_mut().zip(&self.drivers) {
            if !gba.load_rom(path, Some(*driver)) {
                return Err("Failed to load ROM in one or more instances".into());
            }
        }
        drop(guards);
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

    /// Is a given instance (0-based) currently waiting on the link (skipped by the
    /// frame loop)? Used by tests that drive frames directly.
    pub fn instance_sleeping(&self, i: usize) -> bool {
        self.sleeping_flags.lock().unwrap()[i]
    }

    /// Start the emulation loop. Emulation always runs at ~60 FPS so the games keep
    /// correct speed and the lockstep link stays in sync; video frames are only built
    /// and broadcast `video_fps` times per second, decoupled from emulation so lowering
    /// the video rate never slows the game or breaks the link.
    pub fn start(&self, video_fps: u32) {
        let instances = self.instances.clone();
        let sleeping = self.sleeping_flags.clone();
        let tx = self.frame_sender.clone();

        thread::spawn(move || {
            let mut last_frame = Instant::now();
            let frame_duration = Duration::from_micros(16666); // ~60 FPS emulation
            let video_every = (60 / video_fps.clamp(1, 60)).max(1);
            let mut tick = 0u32;

            loop {
                {
                    let mut guards: Vec<_> =
                        instances.iter().map(|i| i.lock().unwrap()).collect();

                    if guards.iter().all(|g| g.is_running) {
                        // Skip players waiting on the link (their frame ended early at
                        // the sync point); they resume once the other player wakes them.
                        for i in 0..guards.len() {
                            if sleeping.lock().unwrap()[i] {
                                continue;
                            }
                            guards[i].run_frame();
                        }

                        tick = tick.wrapping_add(1);
                        if tick % video_every == 0 {
                            // RGBA8888: 4 bytes/pixel so the frontend can putImageData
                            // directly with no per-pixel decode.
                            let mut combined = Vec::with_capacity(240 * 160 * 4 * guards.len());
                            for gba in guards.iter() {
                                combined.extend_from_slice(&gba.get_pixels_rgba());
                            }
                            let _ = tx.send(combined);
                        }
                    }
                }

                let elapsed = last_frame.elapsed();
                if elapsed < frame_duration {
                    thread::sleep(frame_duration - elapsed);
                }
                last_frame = Instant::now();
            }
        });
    }
}
