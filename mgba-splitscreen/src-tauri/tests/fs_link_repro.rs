//! Four Swords link repro — native counterpart of the browser flow.
//!
//! The browser WASM drives the same cooperative model as `EmulationManager`
//! (every instance stepped on one thread, a sleeping primary skipped), so this
//! test reproduces a browser session without a WASM rebuild: load the FS ROM
//! into 4 linked instances, import the user's captured state set (captured at
//! the FS link screen), then press START on all four and let it run.
//!
//! The FS assist/kick logs (WARN) print to stdout, so run it with
//! `cargo test --release --test fs_link_repro -- --ignored --nocapture` and
//! read the `FS kick declined` / `kicked deadlock` / `MULTI did not receive`
//! lines. `#[ignore]` keeps it out of the normal suite (it sleeps ~12s).

use std::time::Duration;

use mgba_splitscreen_lib::emulation::EmulationManager;

/// mGBA's GBA key masks (see gba/interface.h GBAKey).
const KEY_START: u32 = 0x0008;
const KEY_A: u32 = 0x0001;
const KEY_B: u32 = 0x0002;

fn test_dir() -> std::path::PathBuf {
    for candidate in ["../../Test Roms", "Test Roms", "../Test Roms"] {
        let dir = std::path::Path::new(candidate);
        if dir.is_dir() {
            return dir.to_path_buf();
        }
    }
    panic!("Test Roms/ not found");
}

fn fs_rom() -> String {
    let dir = test_dir();
    for entry in std::fs::read_dir(&dir).unwrap().flatten() {
        let name = entry.file_name().to_string_lossy().to_string();
        if name.ends_with(".gba") && name.contains("Four Swords") {
            return entry.path().to_str().unwrap().to_string();
        }
    }
    panic!("Four Swords ROM not found in {}", dir.display());
}

#[test]
#[ignore = "manual repro; sleeps ~12s and needs Test Roms/"]
fn fs_link_state_import_with_start() {
    // FS_LINK_PLAYERS / FS_LINK_STATE let the same repro run the 2-player
    // control (the configuration that previously reached gameplay) without a
    // second test body.
    let players: usize = std::env::var("FS_LINK_PLAYERS")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(4)
        .clamp(2, 4);
    let state_name = std::env::var("FS_LINK_STATE").unwrap_or_else(|_| "mgba-splitscreen.dualbystate".into());
    let tag = std::env::var("FS_LINK_TAG").unwrap_or_else(|_| "r".into());

    let rom = fs_rom();
    let state_path = test_dir().join(&state_name);
    let state = std::fs::read(&state_path)
        .unwrap_or_else(|e| panic!("read {}: {e}", state_path.display()));
    eprintln!("ROM: {rom}");
    eprintln!("players: {players}  state: {} ({} bytes)", state_path.display(), state.len());

    let mgr = EmulationManager::new(players);
    mgr.load_rom(&rom).expect("load ROM into all instances");
    mgr.load_state_set(&state).expect("import state set");

    // Start the real frame loop (same cooperative stepping as the web build).
    mgr.start(60);

    // Let the imported games settle, then press START on all four exactly as a
    // user does (solo-keyboard mode: space per active player).
    std::thread::sleep(Duration::from_millis(800));
    for p in 1..=players as u8 {
        mgr.set_keys(p, KEY_START).expect("set START");
    }
    std::thread::sleep(Duration::from_millis(250));
    for p in 1..=players as u8 {
        mgr.set_keys(p, 0).expect("release START");
    }

    // Link-screen confirmation (2026-09-10, harness recipe): the mode-9 link
    // screen waits for A-presses -- sub-state 3 needs A to START linking,
    // sub-state 4 needs A again (after >120 frames of active link) to confirm
    // -- so tap A on every player every 10s, spam-safe per the harness (the
    // games' own sub-state machine ignores premature presses). A+B does NOT
    // confirm: it cancels the link back to the title (observed in the repro
    // screenshots).
    let first_a_ms: u64 = std::env::var("FS_LINK_FIRST_A_MS")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(3000);
    std::thread::sleep(Duration::from_millis(first_a_ms));
    dump_screens(&mgr, &format!("{tag}c"));
    eprintln!("tapping A on all players");
    for p in 1..=players as u8 {
        mgr.set_keys(p, KEY_A).expect("set A");
    }
    std::thread::sleep(Duration::from_millis(200));
    for p in 1..=players as u8 {
        mgr.set_keys(p, 0).expect("release A");
    }

    // Watch ~70s tapping A every 10s (the harness's post-link watch does the
    // same). Dump each player's screen at several points so the run can be
    // judged visually (link screen / name entry / char select / gameplay).
    for round in 0..7 {
        std::thread::sleep(Duration::from_secs(10));
        dump_screens(&mgr, &format!("{tag}{}", (b'a' + round) as char));
        eprintln!("tapping A on all players (round {round})");
        for p in 1..=players as u8 {
            mgr.set_keys(p, KEY_A).expect("set A");
        }
        std::thread::sleep(Duration::from_millis(200));
        for p in 1..=players as u8 {
            mgr.set_keys(p, 0).expect("release A");
        }
    }
    mgr.stop_and_join();
}

/// Write each player's RGBA frame as a PPM so the run can be inspected.
fn dump_screens(mgr: &EmulationManager, tag: &str) {
    for (i, inst) in mgr.instances.iter().enumerate() {
        let g = inst.lock().unwrap();
        let px = g.get_pixels_rgba();
        let path = format!("/tmp/fs_repro_{tag}_p{}.ppm", i + 1);
        let mut out = Vec::with_capacity(px.len() / 4 * 3 + 32);
        out.extend_from_slice(format!("P6\n240 160\n255\n").as_bytes());
        for p in px.chunks_exact(4) {
            out.extend_from_slice(&p[0..3]);
        }
        std::fs::write(&path, out).unwrap();
        eprintln!("wrote {path}");
    }
}
