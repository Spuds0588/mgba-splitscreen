use std::ffi::CString;
use std::sync::{Arc, Mutex};
use std::ptr;
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

    pub fn load_rom(&mut self, path: &str) -> bool {
        // Use a relative path in the current working directory
        let temp_name = format!("temp_instance_{}.gba", self.id);
        println!("[GBA {}] Copying ROM to local file: {}...", self.id, temp_name);
        if let Err(e) = std::fs::copy(path, &temp_name) {
            println!("[GBA {}] Failed to copy ROM: {}", self.id, e);
            return false;
        }

        let c_path = CString::new(temp_name.as_str()).unwrap();
        
        unsafe {
            println!("[GBA {}] Step 1: Preloading ROM file...", self.id);
            if bindings::mCorePreloadFile(self.core, c_path.as_ptr()) {
                println!("[GBA {}] Step 2: ROM preloaded. Initializing hardware...", self.id);
                
                if let Some(init_fn) = (*self.core).init {
                    init_fn(self.core);
                }

                println!("[GBA {}] Step 3: Setting video buffer...", self.id);
                if let Some(set_video_buffer_fn) = (*self.core).setVideoBuffer {
                    set_video_buffer_fn(self.core, self.video_buffer.as_mut_ptr() as *mut _, 240);
                }
                
                println!("[GBA {}] Step 4: Loading config...", self.id);
                bindings::mCoreInitConfig(self.core, std::ptr::null_mut());
                bindings::mCoreLoadConfig(self.core);
                
                println!("[GBA {}] Step 5: Resetting core...", self.id);
                if let Some(reset_fn) = (*self.core).reset {
                    reset_fn(self.core);
                }
                
                self.is_running = true;
                println!("[GBA {}] GBA instance is now RUNNING.", self.id);
                
                // Cleanup temp file after load
                let _ = std::fs::remove_file(temp_name);
                true
            } else {
                println!("[GBA {}] mCorePreloadFile failed.", self.id);
                let _ = std::fs::remove_file(temp_name);
                false
            }
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
