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
  WebSocket server (`ws://127.0.0.1:8088`) that pushes frames to the frontend **and**
  accepts the same input protocol as the web demo:
  `{"type":"load_rom","path":"..."}` / `{"type":"keys","player":N,"keys":bits}`.
  This makes the desktop app drivable headlessly (see `scripts/ws_play.py`).
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
- [x] 2–4 synchronized instances with lockstep link cable (virtual link).
- [x] WebSocket frame streaming to a multi-canvas UI.
- [x] P1/P2 keyboard input mapping (laptop-optimized).
- [x] Fixed several crashes: ROM load init order, bindgen/layout mismatch,
      dangling coordinator pointer, NULL `mLockstepUser`, teardown order.
- [x] Battery-save import/export (per instance + combined save set).
- [x] Web demo: standalone server (`cargo run --bin dualboy-web -- --players N`)
      serving the same frontend in a browser over HTTP + WebSocket.
- [x] Headless smoke tests against `Test Roms/` (load + render + save round-trip).
- [x] RGB565 frame streaming (2 bytes/pixel) to halve bandwidth for low-end devices.
- [x] **Desktop app verified end-to-end on a real display (Xvfb/XWayland)**: window
      renders, ROM loads through the real GTK file dialog, game runs with live
      animation (112k px differ between frames 2s apart), and lockstep sync is
      active ("Primary waiting for players to ack" / "All players acked").
- [x] **Played into real games via automated input** (2026-08-16, via `ws_play.py`):
      - *Four Swords* (ALttP/FS cart): booted → file select → created a new save
        (typed name `AAAA` through the on-screen keyboard) → `CHOOSE A GAME` → selected
        Four Swords → reached the multi-pak connection screen with heavy lockstep sync
        (1300+ "waiting for players to ack" events).
      - *A Link to the Past*: selected it at `CHOOSE A GAME` → Triforce intro running
        and animating.
      - *Super Mario Advance 4*: past the intro to the SMB3 title screen.
      - *Shining Soul II*: boots and renders (192 colors, full screen).
      All three test ROMs load, render, and animate; no crashes across all runs.

In progress / next:
- [ ] Audio routing (both instances' audio to the output).
- [ ] Further perf: delta/region compression, buffer reuse (if profiling shows need).
- [ ] Gamepad support for players 3–4 in the browser (Gamepad API).
- [ ] Reduce mGBA debug log spam in the console.

## Headless desktop-app verification (how we tested it)

The Tauri app was verified on a headless Linux box with a virtual display
(`DISPLAY=:1`, XWayland under KDE). No new system packages were needed: WebKitGTK,
GTK3, librsvg and libsoup were already present. Tooling used (all in the gitignored
`.freebuff/` scratch dir):

- `pip install --target .freebuff/pylibs python-xlib` (pure-Python, no root) for
  XTEST synthetic input + XGetImage window grabs.
- `DualBoy/scripts/gui_smoke.py` (committed copy of `.freebuff/drive.py`) — drives the
  app: focuses the window, scrolls the webview with the End key (wheel events don't
  reach it), clicks the teal `Load ROM` button (found by grabbing the window and
  locating teal pixels), then in the GTK `Open File` dialog: Ctrl+L → paste the
  directory path via an X11 CLIPBOARD selection owner + Ctrl+V → Enter, then Escape,
  End x2 (selects the last row — the oldest ROM), then Enter to open. Full usage in
  `DualBoy/scripts/README.md`.

Gotchas discovered while writing it (useful if you redo this):
- This XWayland's core keyboard map has ONE keysym per keycode, so typing punctuation
  or shifted characters via XTEST is unreliable → use clipboard paste instead.
- The window's absolute origin must come from `root.translate_coords(win, 0, 0)`;
  `win.translate_coords(root, 0, 0)` returns the inverse.
- The GTK dialog is titled `Open File`; `xwd -id` captures it, and `ffmpeg` converts
  xwd → png (PIL can't read this xwd variant).
- Screenshot proof: `xwd -id <win> a.xwd`, wait 2s, `b.xwd`, then `ffmpeg`-convert and
  diff with PIL → 112,515 differing pixels proves live rendering.

### Deterministic gameplay driving (preferred)

Driving the GTK dialog with synthetic X11 events is flaky, so `DualBoy/scripts/ws_play.py`
drives the emulator directly over the app's WebSocket instead — load a ROM, inject GBA
button inputs per player, read the real emulated frames back, and verify animation. This
is deterministic and needs no display. Example (see `DualBoy/scripts/README.md`):

```bash
./target/debug/dualboy &   # starts ws://127.0.0.1:8088
python3 DualBoy/scripts/ws_play.py "Test Roms/Legend of Zelda, The - A Link To The Past Four Swords (U) [!].gba" \
  --boot 12 --seq "A WAIT:2500 A A A A WAIT:600 START WAIT:600 A WAIT:2500 A WAIT:2500 A"
```

Sequence tokens are `[P<N>:]BUTTON[:hold_ms]` (e.g. `P2:START`), plus `WAIT:ms`.
This is what proved the full boot→new-save→name-entry→game-select→play flow above.

## Build & run

```bash
cd DualBoy
npm install
npm run tauri dev            # desktop dev (needs a display)

cd src-tauri
cargo build                  # build everything
cargo test --test smoke      # headless emulation + save round-trip
cargo run --bin dualboy-web -- --players 4   # web demo on http://127.0.0.1:8080
```

Build notes: first `libmgba` build takes minutes and several GB RAM (needs cmake + clang).
This workspace has 15 GB RAM / 8 cores, which is plenty.

## Handoff notes for future sessions

- Read this file + `DualBoy/src-tauri/src/*.rs` and `DualBoy/src/main.js`; the rest of
  the repo is upstream mGBA.
- Commit + push frequently (history has been lost to VM OOM before).
- Follow YAGNI; prefer small, single-purpose changes.
