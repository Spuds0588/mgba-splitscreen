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
}

impl EmulationManager {
    pub fn new() -> Self {
        let (tx, _) = broadcast::channel(10);
        let mut coordinator = unsafe { std::mem::zeroed::<bindings::GBASIOLockstepCoordinator>() };
        unsafe {
            bindings::GBASIOLockstepCoordinatorInit(&mut coordinator);
        }

        let mut gba1 = GbaInstance::new();
        let mut gba2 = GbaInstance::new();

        unsafe {
            let driver1 = Box::into_raw(Box::new(std::mem::zeroed::<bindings::GBASIOLockstepDriver>()));
            let driver2 = Box::into_raw(Box::new(std::mem::zeroed::<bindings::GBASIOLockstepDriver>()));

            bindings::GBASIOLockstepDriverCreate(driver1, ptr::null_mut());
            bindings::GBASIOLockstepDriverCreate(driver2, ptr::null_mut());

            bindings::GBASIOLockstepCoordinatorAttach(&mut coordinator, driver1);
            bindings::GBASIOLockstepCoordinatorAttach(&mut coordinator, driver2);

            gba1.set_sio_driver(driver1);
            gba2.set_sio_driver(driver2);
        }

        Self {
            instance1: Arc::new(Mutex::new(gba1)),
            instance2: Arc::new(Mutex::new(gba2)),
            frame_sender: tx,
            coordinator: Arc::new(Mutex::new(coordinator)),
        }
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

                    gba1.run_frame();
                    gba2.run_frame();

                    let mut combined = Vec::with_capacity(240 * 160 * 4 * 2);
                    combined.extend_from_slice(&gba1.get_pixels_raw());
                    combined.extend_from_slice(&gba2.get_pixels_raw());

                    let _ = tx.send(combined);
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

unsafe impl Send for bindings::GBASIOLockstepCoordinator {}
unsafe impl Sync for bindings::GBASIOLockstepCoordinator {}
