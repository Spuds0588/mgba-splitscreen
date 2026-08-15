use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};
use tokio::sync::broadcast;
use crate::gba::GbaInstance;
use crate::bindings;

// The lockstep driver calls user->sleep/wake to block/resume the host thread in mGBA's
// threaded model. Here both instances run sequentially on one thread, so the frame-level
// pause (`nextEvent = 0` + interrupt) is what actually stops a frame; sleep/wake just
// need to be non-NULL no-ops.
unsafe extern "C" fn lockstep_noop(_user: *mut bindings::mLockstepUser) {}

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
    pub instance1: Arc<Mutex<GbaInstance>>,
    pub instance2: Arc<Mutex<GbaInstance>>,
    pub frame_sender: broadcast::Sender<Vec<u8>>,
    driver1: *mut bindings::GBASIOLockstepDriver,
    driver2: *mut bindings::GBASIOLockstepDriver,
    _user1: Box<bindings::mLockstepUser>,
    _user2: Box<bindings::mLockstepUser>,
    // Boxed so its address is stable: the lockstep drivers hold a raw pointer to it.
    coordinator: LockstepCoordinator,
}

unsafe impl Send for EmulationManager {}
unsafe impl Sync for EmulationManager {}
unsafe impl Send for bindings::GBASIOLockstepCoordinator {}
unsafe impl Sync for bindings::GBASIOLockstepCoordinator {}

impl EmulationManager {
    pub fn new() -> Self {
        println!("Creating EmulationManager...");
        let (tx, _) = broadcast::channel(10);
        let mut coordinator = Box::new(unsafe { std::mem::zeroed::<bindings::GBASIOLockstepCoordinator>() });
        unsafe {
            bindings::GBASIOLockstepCoordinatorInit(&mut *coordinator);
        }

        let mut gba1 = GbaInstance::new(1);
        let mut gba2 = GbaInstance::new(2);

        let (d1, d2, user1, user2) = unsafe {
            let d1_ptr = Box::into_raw(Box::new(std::mem::zeroed::<bindings::GBASIOLockstepDriver>()));
            let d2_ptr = Box::into_raw(Box::new(std::mem::zeroed::<bindings::GBASIOLockstepDriver>()));

            let mut user1 = Box::new(std::mem::zeroed::<bindings::mLockstepUser>());
            let mut user2 = Box::new(std::mem::zeroed::<bindings::mLockstepUser>());
            user1.sleep = Some(lockstep_noop);
            user1.wake = Some(lockstep_noop);
            user2.sleep = Some(lockstep_noop);
            user2.wake = Some(lockstep_noop);

            bindings::GBASIOLockstepDriverCreate(d1_ptr, &mut *user1);
            bindings::GBASIOLockstepDriverCreate(d2_ptr, &mut *user2);

            bindings::GBASIOLockstepCoordinatorAttach(&mut *coordinator, d1_ptr);
            bindings::GBASIOLockstepCoordinatorAttach(&mut *coordinator, d2_ptr);

            (d1_ptr, d2_ptr, user1, user2)
        };

        EmulationManager {
            instance1: Arc::new(Mutex::new(gba1)),
            instance2: Arc::new(Mutex::new(gba2)),
            frame_sender: tx,
            driver1: d1,
            driver2: d2,
            _user1: user1,
            _user2: user2,
            coordinator: LockstepCoordinator(coordinator),
        }
    }

    pub fn attach_drivers(&self) {
        let mut gba1 = self.instance1.lock().unwrap();
        let mut gba2 = self.instance2.lock().unwrap();
        gba1.set_sio_driver(self.driver1);
        gba2.set_sio_driver(self.driver2);
    }

    pub fn start(&self) {
        let inst1 = self.instance1.clone();
        let inst2 = self.instance2.clone();
        let tx = self.frame_sender.clone();

        thread::spawn(move || {
            let mut last_frame = Instant::now();
            let frame_duration = Duration::from_micros(16666); // ~60 FPS

            loop {
                {
                    let mut gba1 = inst1.lock().unwrap();
                    let mut gba2 = inst2.lock().unwrap();

                    if gba1.is_running && gba2.is_running {
                        gba1.run_frame();
                        gba2.run_frame();

                        let mut combined = Vec::with_capacity(240 * 160 * 4 * 2);
                        combined.extend_from_slice(&gba1.get_pixels_raw());
                        combined.extend_from_slice(&gba2.get_pixels_raw());

                        let _ = tx.send(combined);
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


