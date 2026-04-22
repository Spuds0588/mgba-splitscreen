use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};
use std::ptr;
use tokio::sync::broadcast;
use crate::gba::GbaInstance;
use crate::bindings;

pub struct EmulationManager {
    pub instance1: Arc<Mutex<GbaInstance>>,
    pub instance2: Arc<Mutex<GbaInstance>>,
    pub frame_sender: broadcast::Sender<Vec<u8>>,
    coordinator: Arc<Mutex<bindings::GBASIOLockstepCoordinator>>,
    driver1: *mut bindings::GBASIOLockstepDriver,
    driver2: *mut bindings::GBASIOLockstepDriver,
}

unsafe impl Send for EmulationManager {}
unsafe impl Sync for EmulationManager {}
unsafe impl Send for bindings::GBASIOLockstepCoordinator {}
unsafe impl Sync for bindings::GBASIOLockstepCoordinator {}

impl EmulationManager {
    pub fn new() -> Self {
        println!("Creating EmulationManager...");
        let (tx, _) = broadcast::channel(10);
        let mut coordinator = unsafe { std::mem::zeroed::<bindings::GBASIOLockstepCoordinator>() };
        unsafe {
            bindings::GBASIOLockstepCoordinatorInit(&mut coordinator);
        }

        let mut gba1 = GbaInstance::new(1);
        let mut gba2 = GbaInstance::new(2);

        let (d1, d2) = unsafe {
            let d1_ptr = Box::into_raw(Box::new(std::mem::zeroed::<bindings::GBASIOLockstepDriver>()));
            let d2_ptr = Box::into_raw(Box::new(std::mem::zeroed::<bindings::GBASIOLockstepDriver>()));

            bindings::GBASIOLockstepDriverCreate(d1_ptr, ptr::null_mut());
            bindings::GBASIOLockstepDriverCreate(d2_ptr, ptr::null_mut());

            bindings::GBASIOLockstepCoordinatorAttach(&mut coordinator, d1_ptr);
            bindings::GBASIOLockstepCoordinatorAttach(&mut coordinator, d2_ptr);

            // gba1.set_sio_driver(d1_ptr);
            // gba2.set_sio_driver(d2_ptr);
            (d1_ptr, d2_ptr)
        };

        EmulationManager {
            instance1: Arc::new(Mutex::new(gba1)),
            instance2: Arc::new(Mutex::new(gba2)),
            frame_sender: tx,
            coordinator: Arc::new(Mutex::new(coordinator)),
            driver1: d1,
            driver2: d2,
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

impl Drop for EmulationManager {
    fn drop(&mut self) {
        unsafe {
            let mut coord = self.coordinator.lock().unwrap();
            bindings::GBASIOLockstepCoordinatorDeinit(&mut *coord);
        }
    }
}

