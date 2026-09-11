//! Audio probe: load a ROM through the exact same GbaInstance path the app
//! uses, run frames, and report the audio ring's peak amplitude. Answers
//! "does the core produce sound in this harness at all?" Run with:
//! `cargo run --release --bin audioprobe -- <rom.gba> [frames=300]`

use std::time::Instant;
use dualboy_lib::gba::GbaInstance;

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let rom = args.get(1).expect("usage: audioprobe <rom.gba> [frames=300]");
    let frames: usize = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(300);

    let mut g = GbaInstance::new(1);
    if !g.load_rom(rom, None) {
        eprintln!("failed to load {rom}");
        std::process::exit(1);
    }

    // Warm up through boot (BIOS-ish ~120 frames) so we measure a live game.
    for _ in 0..120 {
        g.run_frame();
    }

    let start = Instant::now();
    let mut peak: u32 = 0;
    let mut nonzero = 0usize;
    let mut total = 0usize;
    let mut frames_with_audio = 0usize;
    for _ in 0..frames {
        g.run_frame();
        let s = g.drain_audio();
        total += s.len();
        if s.iter().any(|v| *v != 0) {
            frames_with_audio += 1;
            nonzero += s.iter().filter(|v| **v != 0).count();
        }
        for v in s {
            peak = peak.max(v.unsigned_abs() as u32);
        }
    }
    let dt = start.elapsed().as_secs_f64();
    println!("rom={rom} frames={frames} in {dt:.2}s ({:.1} fps)", frames as f64 / dt);
    println!("audio: total samples={total}, frames-with-audio={frames_with_audio}/{frames}, nonzero={nonzero}, peak={peak}");
    if peak == 0 {
        println!("RESULT: CORE PRODUCES SILENCE in this harness");
    } else {
        println!("RESULT: CORE PRODUCES AUDIO (peak={peak})");
    }
}
