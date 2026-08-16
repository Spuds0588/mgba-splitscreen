//! Headless smoke tests: verify the full emulation pipeline (ROM load -> run frames
//! -> pixels rendered) and save import/export, using the ROMs in `Test Roms/`.

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

    let mgr = EmulationManager::new(2);
    mgr.load_rom(&rom).expect("load ROM into all instances");

    // Run a couple seconds of frames; the BIOS/title screen should render pixels.
    // (Skip instances currently waiting on the link cable, like the frame loop does.)
    for _ in 0..60 {
        let mut guards: Vec<_> = mgr.instances.iter().map(|i| i.lock().unwrap()).collect();
        for i in 0..guards.len() {
            if mgr.instance_sleeping(i) {
                continue;
            }
            guards[i].run_frame();
        }
    }

    let pixels = mgr.instances[0].lock().unwrap().get_pixels_rgba();
    let any_rendered = pixels
        .chunks_exact(4)
        .any(|px| px[0] != 0 || px[1] != 0 || px[2] != 0);
    assert!(any_rendered, "no pixels rendered after 60 frames");
}

#[test]
fn save_round_trip() {
    let rom = find_test_rom();
    let mgr = EmulationManager::new(2);
    mgr.load_rom(&rom).expect("load ROM");

    // Run a few seconds of frames so the game initializes its save memory
    // (frames can be skipped while an instance waits on the link cable).
    for _ in 0..180 {
        let mut guards: Vec<_> = mgr.instances.iter().map(|i| i.lock().unwrap()).collect();
        for i in 0..guards.len() {
            if mgr.instance_sleeping(i) {
                continue;
            }
            guards[i].run_frame();
        }
    }

    let save = mgr.export_save(1).expect("export save from player 1");
    assert!(!save.is_empty(), "exported save is empty");
    mgr.import_save(2, &save).expect("import save into player 2");
}
