//! Headless smoke test: verify the full emulation pipeline (ROM load -> run frames
//! -> pixels rendered) without a display, using the ROMs in `Test Roms/`.

use dualboy_lib::emulation::EmulationManager;

/// Find the first `.gba` ROM in the (gitignored) `Test Roms/` directory.
fn find_test_rom() -> String {
    for candidate in ["../../Test Roms", "Test Roms"] {
        let dir = std::path::Path::new(candidate);
        if let Ok(entries) = std::fs::read_dir(dir) {
            for entry in entries.flatten() {
                let path = entry.path();
                if path.extension().and_then(|e| e.to_str()) == Some("gba") {
                    return path.to_str().unwrap().to_string();
                }
            }
        }
    }
    panic!("No .gba ROM found in Test Roms/ — expected the owner's test ROMs there");
}

#[test]
fn loads_rom_and_renders_frames() {
    let rom = find_test_rom();
    eprintln!("Using test ROM: {rom}");

    let mgr = EmulationManager::new();

    // Load the same ROM into both instances and attach the link-cable drivers.
    let loaded = {
        let mut gba1 = mgr.instance1.lock().unwrap();
        let mut gba2 = mgr.instance2.lock().unwrap();
        gba1.load_rom(&rom) && gba2.load_rom(&rom)
    };
    assert!(loaded, "failed to load ROM into both instances");
    mgr.attach_drivers();

    // Run a couple seconds of frames; the BIOS/title screen should render pixels.
    for _ in 0..60 {
        let mut gba1 = mgr.instance1.lock().unwrap();
        let mut gba2 = mgr.instance2.lock().unwrap();
        gba1.run_frame();
        gba2.run_frame();
    }

    let pixels = mgr.instance1.lock().unwrap().get_pixels_raw();
    let any_rendered = pixels
        .chunks_exact(4)
        .any(|px| px[0] != 0 || px[1] != 0 || px[2] != 0);
    assert!(any_rendered, "no pixels rendered after 60 frames");
}
