use std::ffi::CString;
use std::ptr;
use crate::bindings;

pub struct GbaInstance {
    pub core: *mut bindings::mCore,
    video_buffer: Vec<u32>,
}

impl GbaInstance {
    pub fn new() -> Self {
        unsafe {
            let core = bindings::mCoreCreate(bindings::mPlatform_mPLATFORM_GBA);
            if core.is_null() {
                panic!("Failed to create mGBA core");
            }
            
            // Initialize the core
            if let Some(init_fn) = (*core).init {
                init_fn(core);
            }

            // Setup video buffer (240x160)
            let video_buffer = vec![0u32; 240 * 160];
            if let Some(set_video_buffer_fn) = (*core).setVideoBuffer {
                set_video_buffer_fn(core, video_buffer.as_ptr() as *mut _, 240);
            }

            // Default config
            bindings::mCoreInitConfig(core, ptr::null());

            GbaInstance {
                core,
                video_buffer,
            }
        }
    }

    pub fn load_rom(&mut self, path: &str) -> bool {
        let c_path = CString::new(path).unwrap();
        unsafe {
            bindings::mCoreLoadFile(self.core, c_path.as_ptr())
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
            if let Some(set_periph_fn) = (*self.core).setPeripheral {
                // mPERIPH_GBA_LINK_PORT is 0x1001
                set_periph_fn(self.core, 0x1001, driver as *mut _);
            }
        }
    }

    pub fn set_keys(&mut self, keys: u32) {
        unsafe {
            if let Some(set_keys_fn) = (*self.core).setKeys {
                set_keys_fn(self.core, keys);
            }
        }
    }

    pub fn get_pixels_raw(&self) -> Vec<u8> {
        let mut pixels = Vec::with_capacity(240 * 160 * 4);
        for &pixel in &self.video_buffer {
            let r = (pixel & 0xFF) as u8;
            let g = ((pixel >> 8) & 0xFF) as u8;
            let b = ((pixel >> 16) & 0xFF) as u8;
            pixels.push(r);
            pixels.push(g);
            pixels.push(b);
            pixels.push(255); // Alpha
        }
        pixels
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
