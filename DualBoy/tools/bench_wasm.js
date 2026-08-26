/*
 * bench_wasm.js — in-page benchmark for the WASM engine.
 *
 * Loads a fresh module, inits N linked players, loads a real ROM, runs M frames
 * and reports the per-frame db_run_frame() time distribution (avg / p50 / p95 /
 * p99 / max) plus how the time is split between emulation and the RGBA pack.
 *
 * Usage (in the page): set window.__BENCH = { players, rom, frames } then eval
 * this file (it returns a promise with the results).
 */

(async () => {
  const BASE = location.href.replace(/\/[^/]*$/, '/');
  const cfg = window.__BENCH || { players: 4, rom: 'roms/Mario Kart - Super Circuit (USA).gba', frames: 600 };
  const W = 240, H = 160;

  const factory = window.DualBoyWasm;
  const M = await factory({ locateFile: (p) => BASE + 'dualboy-web.wasm' });
  M._db_init(cfg.players);
  const rom = await (await fetch(BASE + cfg.rom)).arrayBuffer();
  const bytes = new Uint8Array(rom);
  const ptr = M._malloc(bytes.length);
  M.HEAPU8.set(bytes, ptr);
  const rc = M._db_load_rom(ptr, bytes.length);
  M._free(ptr);
  if (rc !== 0) return { error: 'load_rom rc=' + rc };

  // Warm up (negotiation settles in the first ~60 frames).
  for (let f = 0; f < 90; f++) M._db_run_frame();

  const times = [];
  const total0 = performance.now();
  for (let f = 0; f < cfg.frames; f++) {
    const t0 = performance.now();
    M._db_run_frame();
    const t1 = performance.now();
    times.push(t1 - t0);
  }
  const totalMs = performance.now() - total0;

  times.sort((a, b) => a - b);
  const p = (q) => times[Math.min(times.length - 1, Math.floor(times.length * q))];
  const sum = times.reduce((a, b) => a + b, 0);

  // Frame-rate the emulation itself could sustain if the browser were infinite.
  const fps = 1000 / (sum / times.length);

  M._db_quit();
  return {
    rom: cfg.rom,
    players: cfg.players,
    frames: cfg.frames,
    totalMs: +totalMs.toFixed(1),
    avgMs: +(sum / times.length).toFixed(3),
    p50: +p(0.5).toFixed(3),
    p95: +p(0.95).toFixed(3),
    p99: +p(0.99).toFixed(3),
    max: +p(1).toFixed(3),
    fps,
    budgetMs: 1000 / 60,
  };
})()
