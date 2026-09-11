//! 4-player SIO ground truth through the linktest instrument ROM.
//!
//! `fs_link_repro` shows what Four Swords does; this shows what the *link
//! layer* does, with a ROM written to report it: `mgba-splitscreen/linktest/main.c`
//! programs the link port in MULTI mode and renders, live, all four SIOMULTI
//! slots, TX/RX transfer counters, the per-device stall counter and each
//! device's frame counter.
//!
//! It runs through `EmulationManager`, which is the same cooperative stepping
//! model the browser WASM uses (every instance stepped on one thread, a
//! sleeping primary skipped), so a failure here is a failure in the browser.
//!
//!   cargo test --release --test linktest_4p -- --ignored --nocapture
//!
//! Set `LINKTEST_PLAYERS` to choose the unit count (default 4).

use std::time::Duration;

use mgba_splitscreen_lib::emulation::EmulationManager;

fn linktest_rom() -> String {
    for candidate in [
        "../linktest/linktest.gba",
        "mgba-splitscreen/linktest/linktest.gba",
        "../../mgba-splitscreen/linktest/linktest.gba",
    ] {
        let p = std::path::Path::new(candidate);
        if p.is_file() {
            return p.to_str().unwrap().to_string();
        }
    }
    panic!("linktest.gba not found relative to {:?}", std::env::current_dir());
}

#[test]
#[ignore = "manual ground-truth run; needs mgba-splitscreen/linktest/linktest.gba"]
fn linktest_four_players_cooperative() {
    let players: usize = std::env::var("LINKTEST_PLAYERS")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(4)
        .clamp(2, 4);
    let seconds: u64 = std::env::var("LINKTEST_SECONDS")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(20);
    let tag = std::env::var("LINKTEST_TAG").unwrap_or_else(|_| "lt".into());

    let rom = linktest_rom();
    eprintln!("linktest ROM: {rom}  players: {players}  seconds: {seconds}");

    let mgr = EmulationManager::new(players);
    mgr.load_rom(&rom).expect("load linktest into all instances");
    mgr.start(60);

    // The ROM boots, draws its static labels, then enters MULTI mode and starts
    // exchanging. Give it a moment to settle before the first sample.
    std::thread::sleep(Duration::from_secs(3));
    for step in 0.. {
        if step as u64 * 3 >= seconds {
            break;
        }
        let frms: Vec<u32> = mgr
            .instances
            .iter()
            .map(|i| i.lock().unwrap().frame_counter())
            .collect();
        let sleeping: Vec<bool> = (0..players).map(|p| mgr.instance_sleeping(p)).collect();
        eprintln!(
            "[{:>2}s] frames={:?} sleeping={:?}",
            step * 3,
            frms,
            sleeping
        );
        dump_screens(&mgr, &format!("{tag}{step}"));
        std::thread::sleep(Duration::from_secs(3));
    }
    mgr.stop_and_join();
}

/// Write each player's RGBA frame as a PPM so the run can be inspected.
fn dump_screens(mgr: &EmulationManager, tag: &str) {
    for (i, inst) in mgr.instances.iter().enumerate() {
        let g = inst.lock().unwrap();
        let px = g.get_pixels_rgba();
        let path = format!("/tmp/linktest_{tag}_p{}.ppm", i + 1);
        let mut out = Vec::with_capacity(px.len() / 4 * 3 + 32);
        out.extend_from_slice(b"P6\n240 160\n255\n");
        for p in px.chunks_exact(4) {
            out.extend_from_slice(&p[0..3]);
        }
        std::fs::write(&path, out).unwrap();
    }
    eprintln!("wrote /tmp/linktest_{tag}_p*.ppm");
}
