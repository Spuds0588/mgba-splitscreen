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
- `DualBoy/src/main.js` — frontend: top menu bar + video-call grid of canvases, RGBA
  frames fed straight into `putImageData`, keyboard → `set_keys` via Tauri invoke or
  the WebSocket.
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
  `GbaInstance::get_pixels_rgba` re-packs it to RGBA8888 (alpha OR'd in) — the exact
  format `putImageData` wants, so the frontend does zero per-pixel decode.
- **Test ROMs**: `Test Roms/` (gitignored) holds the owner's legal ROMs for testing.
- **Lockstep sleep = thread block**: mGBA's lockstep expects the primary's *thread* to
  block inside `user->sleep` until the secondary catches up (the threaded model). DualBoy
  runs all instances sequentially on one thread, so a no-op `sleep` let the primary run
  straight past its transfer-completion event → "MULTI did not receive data" → games
  showed the multi-pak "turn power OFF/ON" screen. Fix (2026-08-16):
  `GBASIOLockstepPlayerSleep` now ends the current frame early
  (`++player->...->video.frameCounter`) and the frame loop skips a player while its sleep
  flag is set (flipped by `user->sleep`/`wake` in `emulation.rs`) — the sequential-model
  equivalent of the primary's thread blocking.
- **Lockstep crawl (2026-08-16, commit e1e98006d)**: ending the frame early in
  `PlayerSleep` must apply to the PRIMARY ONLY (`playerId == 0`). Ending a secondary's
  frame early starves the pipeline — the primary sleeps at the transfer wait while the
  secondary spins and never completes its frame, so emulation crawls at 2–3 FPS even
  though video still broadcasts at 30 FPS. Secondaries keep running to completion so they
  can deliver data and wake the primary. Symptom to watch for: log clean + low CPU + 30
  FPS broadcast but static game content = the wrong player's frame is being ended early.
- **Link attach before boot**: attach the lockstep SIO driver BEFORE `reset`, so the game
  sees the link cable present at boot (mGBA's Qt multiplayer does the same). Attaching
  after boot made the link appear mid-boot.
- **mGBA log spam**: mGBA installs NO default logger, so `mLog()` falls back to printing
  every level (incl. DEBUG BIOS SWI traces + lockstep chatter) to stdout — one short
  browser session produced 179k lines / 11 MB, a large synchronous-I/O drag. Fix:
  `ensure_logger()` in `emulation.rs` installs the standard logger restricted to
  WARN|ERROR|FATAL (one-time, via `Once`).
- **Video vs emulation rate**: emulation must stay at ~60 FPS for correct game speed and
  lockstep sync; video is independent and can be throttled without breaking the link.
  `EmulationManager::start(fps)` decouples them (web server: `--fps`, default 30).

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
- [x] ~~RGB565 frame streaming~~ — replaced by RGBA8888 (2026-08-16): the bandwidth
      saving wasn't worth the per-pixel JS decode it forced on every frame.
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
- [x] **Link driver attached before boot** (2026-08-16) — games now see the virtual
      link cable at boot instead of mid-boot (mGBA's Qt multiplayer ordering).
- [x] **Cooperative lockstep sleep** (2026-08-16) — a sleeping player ends its frame
      early and is skipped by the frame loop until woken, eliminating
      "MULTI did not receive data" desyncs (the root cause of the multi-pak
      "power off/on" screen). `cargo test` green (128 unit + 2 smoke); full two-player
      runs show zero link errors.
- [x] **Performance pass** (2026-08-16): installed a WARN-only default logger (killed the
      ~180k-line BIOS/SIO debug flood), built libmgba in Release (`-O3 -DNDEBUG`), and
      decoupled video from emulation (emulation 60 FPS, video 30 FPS default via
      `--fps`). Verified 29.7 FPS video, a 21-line log, and no crash across 35 s of
      sustained two-player input in both the web server and the Tauri app.
- [x] **Performance pass 2** (2026-08-16): the desktop/browser were still running an
      unoptimized **223 MB debug Rust binary** (no release build existed) and the
      frontend decoded RGB565 per-pixel on every frame. Now: an optimized release
      profile (`opt-level=3`, thin LTO, `strip` → 11 MB binary), RGBA8888 streaming
      straight into `putImageData` (no JS decode), and a video-call layout with a top
      menu bar (no scrolling, no keyboard-focus conflicts). `bench` shows 4 synced
      instances of *Shining Soul II* emulate at ~4,000 FPS on one thread (0.24
      ms/frame) — **emulation is not the bottleneck**, rendering/UI was.

In progress / next:
- [ ] Root-cause the one observed tokio-worker segfault (`segfault at 4a8` in
      `dualboy-web`): suspected cross-thread `load_rom` (tokio) vs `run_frame` (std
      emulation thread), possibly aggravated by the old debug build's log flood. Needs
      sustained-play re-testing now that logging and build profile are fixed.
- [ ] Verify Four Swords *enters* an actual 2-player session end-to-end (the link now
      stays clean with zero desyncs; `adaptive_play.py` menu navigation needs tuning).
- [ ] Audio routing (both instances' audio to the output).
- [ ] Gamepad support for players 3–4 in the browser (Gamepad API).
- [ ] If the WebView still can't composite 30 FPS on low-end hardware, add a native
      (non-webview) renderer for the desktop app. Emulation is ~4,000 FPS, so a native
      canvas/GPU path is the only remaining performance lever.

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
cd DualBoy/src-tauri
cargo build --release        # ALWAYS release: the Rust frame pipeline is ~10x faster
./target/release/dualboy     # desktop window (needs a display)

cargo test --release         # 128 unit + 2 smoke tests (needs Test Roms/)
cargo run --release --bin dualboy-web -- --players 4 --fps 30  # http://127.0.0.1:8080
cargo run --release --bin bench -- <rom.gba> [players]         # emulation speed only
```

Build notes: first `libmgba` build takes minutes and several GB RAM (needs cmake + clang).
This workspace has 15 GB RAM / 8 cores, which is plenty. `--fps` sets the video send rate
(1–60, default 30) independently of emulation speed; always use `cargo build --release`.

## Handoff notes for future sessions

- Read this file + `DualBoy/src-tauri/src/*.rs` and `DualBoy/src/main.js`; the rest of
  the repo is upstream mGBA.
- Commit + push frequently (history has been lost to VM OOM before).
- Follow YAGNI; prefer small, single-purpose changes.
