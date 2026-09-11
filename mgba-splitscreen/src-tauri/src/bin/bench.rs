//! Emulation-speed benchmark: load a ROM into N instances and measure the
//! wall-clock time per frame on one thread (the same model the real frame loop
//! uses). Run with `cargo run --release --bin bench -- <rom> [players]`.
//!
//! This isolates emulation cost from rendering/streaming so we know whether the
//! single-threaded N-instance loop can hold 60 FPS (16.6 ms / N budget), or
//! whether the lockstep instances need to move onto separate threads.

use std::time::Instant;

use dualboy_lib::emulation::EmulationManager;

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let rom = args
        .get(1)
        .expect("usage: bench <rom.gba> [players=2] [frames=600]");
    let players: usize = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(2).clamp(1, 4);
    let frames: usize = args.get(3).and_then(|s| s.parse().ok()).unwrap_or(600);

    let mgr = EmulationManager::new(players);
    mgr.load_rom(rom).expect("load ROM");

    // Warm up through the BIOS boot so we measure steady-state gameplay, not boot.
    for _ in 0..120 {
        let mut guards: Vec<_> = mgr.instances.iter().map(|i| i.lock().unwrap()).collect();
        for i in 0..guards.len() {
            if mgr.instance_sleeping(i) {
                continue;
            }
            guards[i].run_frame();
        }
    }

    let start = Instant::now();
    for _ in 0..frames {
        let mut guards: Vec<_> = mgr.instances.iter().map(|i| i.lock().unwrap()).collect();
        for i in 0..guards.len() {
            if mgr.instance_sleeping(i) {
                continue;
            }
            guards[i].run_frame();
        }
    }
    let ms_per_frame = start.elapsed().as_secs_f64() * 1000.0 / frames as f64;

    println!("players={players} frames={frames}");
    println!("  {ms_per_frame:.2} ms/frame total (all instances, one thread)");
    println!("  = {:.1} FPS emulation vs 60 FPS target", 1000.0 / ms_per_frame);
    println!(
        "  budget for 60 FPS: {:.2} ms/frame ({:.2} ms per instance)",
        1000.0 / 60.0,
        1000.0 / 60.0 / players as f64
    );
}
