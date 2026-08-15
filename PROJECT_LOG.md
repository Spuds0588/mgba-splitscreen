# DualBoy Project Log

## What this is

DualBoy is a **split-screen GBA emulator**: multiple GBA instances running side by side,
synchronized over a virtual link cable (mGBA's lockstep link support), so 2–4 players can
trade/battle/co-op in GBA games on one machine.

This repository is a fork of **mGBA** (`Spuds0588/mgba-splitscreen.git`, which tracks
`mgba-emu/mgba` upstream). The `DualBoy/` directory is the Tauri v2 app built on top of
`libmgba`. Everything outside `DualBoy/` is upstream mGBA source that `libmgba` compiles.

## Architecture (read this first)

- `DualBoy/src-tauri/src/gba.rs` — `GbaInstance`: wraps one `mCore`. Creates a GBA core,
  loads a ROM, runs frames, reads pixels, sets keys, attaches the lockstep SIO driver.
- `DualBoy/src-tauri/src/emulation.rs` — `EmulationManager`: owns two `GbaInstance`s,
  one `GBASIOLockstepCoordinator` + two `GBASIOLockstepDriver`s, and a broadcast channel
  that streams combined frame data (instance1 pixels ++ instance2 pixels) at ~60 FPS.
- `DualBoy/src-tauri/src/lib.rs` — Tauri commands (`load_rom`, `set_keys`) + the
  WebSocket server (`ws://127.0.0.1:8088`) that pushes frames to the frontend.
- `DualBoy/src-tauri/src/bindings.rs` — `include!(OUT_DIR/bindings.rs)` (bindgen output).
- `DualBoy/src-tauri/build.rs` — cmake-builds `libmgba` + runs bindgen over
  `mgba_bindings.h`. NOTE: `mgba_bindings.h` must include
  `<mgba/internal/gba/sio/lockstep.h>` for the `GBASIOLockstep*` API (upstream moved it
  out of the public `core/lockstep.h`).
- `DualBoy/src/main.js` — frontend: canvas rendering + keyboard → `set_keys` via Tauri
  invoke, frames via WebSocket.
- `DualBoy/src-tauri/target/` — build output (gitignored); `libmgba.a` + bindings are
  cached here so incremental builds are fast.

## Key gotchas

- **ROM load order**: mGBA requires `core->init()` BEFORE `mCorePreloadFile()` (which
  calls `core->loadROM`). Loading first dereferences `core->board`, which is NULL until
  `init` — segfault. Correct order: `init → mCoreInitConfig → mCoreLoadConfig →
  setVideoBuffer → mCorePreloadFile → reset`. Fixed 2026-08-15.
- **Lockstep bindings**: after the upstream "migrate includes" merge, `GBASIOLockstep*`
  lives in `include/mgba/internal/gba/sio/lockstep.h`. `mgba_bindings.h` must include it.
- **Pixel format**: `mCore.setVideoBuffer` fills a `u32` buffer in XBGR8888 order;
  `GbaInstance::get_pixels_raw` swizzles to RGBA bytes.
- **Test ROMs**: `Test Roms/` (gitignored) holds the owner's legal ROMs for testing.

## Status

Done:
- [x] Tauri project + Rust bindings + `libmgba` static build.
- [x] Two synchronized instances with lockstep link cable (virtual link).
- [x] WebSocket frame streaming to a dual-canvas UI.
- [x] P1/P2 keyboard input mapping (laptop-optimized).
- [x] ROM loading crash fixed (init order) + lockstep bindings restored.
- [x] Build compiles (`cargo build` in `DualBoy/src-tauri`).

In progress / next:
- [ ] Audio routing (both instances' audio to the output).
- [ ] Save import/export (per instance + as a set).
- [ ] Web version (browser demo, 2–4 players, one ROM) — lost, needs rebuild.
- [ ] Performance optimization for low-end devices.
- [ ] Runtime smoke test against `Test Roms/` (headless) to verify end-to-end emulation.

## Build & run

```bash
cd DualBoy
npm install
npm run tauri dev      # desktop dev (needs a display)
# backend-only compile:
cd src-tauri && cargo build
```

Build notes: first `libmgba` build takes minutes and several GB RAM (needs cmake + clang).
This workspace has 15 GB RAM / 8 cores, which is plenty.

## Handoff notes for future sessions

- Read this file + `DualBoy/src-tauri/src/*.rs` and `DualBoy/src/main.js`; the rest of
  the repo is upstream mGBA.
- Commit + push frequently (history has been lost to VM OOM before).
- Follow YAGNI; prefer small, single-purpose changes.
