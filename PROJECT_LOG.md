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
  lockstep sync; video is decoupled so throttling it never breaks the link. By default
  EVERY emulated frame is broadcast (video 60) so frames render at speed; `--fps` can
  only LOWER the send rate for bandwidth-constrained headless use. The wrapper must
  never drop frames on the producer side — that was the bug where the log counted
  ~58 emu fps but the frontend only ever saw 29.7 (half of every emulated frame was
  thrown away by the integer `video_every` tick). Slow consumers drop on the consumer
  side instead: the tokio broadcast channel drops for lagging receivers, and the
  frontend coalesces to `requestAnimationFrame` showing the latest frame.

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
- [x] **Lockstep crawl fix** (2026-08-16, commit e1e98006d): only the PRIMARY ends its
      frame early at a lockstep sleep; secondaries run to completion so they deliver
      data and wake the primary. Before: both players ended frames early → primary
      asleep + secondary spinning → emulation crawled at 2–3 FPS (clean log, low CPU,
      but static content). After: both players animate continuously (verified 13–14/14
      observation windows), 30 FPS broadcast, zero link errors, 4.7% CPU.
- [x] **Render frames at speed** (2026-08-16): the per-second log exposed the wrapper
      bug — `emu P1=[57.5] P2=[59.5] fps | video:29.7 fps` counted ~58 emulated frames
      but broadcast exactly half (the 30 FPS `video_every=2` default dropped every
      other frame on the producer side). Fixed by broadcasting every emulated frame
      (video default 60 in both the desktop app and web server), replacing the
      per-frame `sleep(16.6 ms − work)` pacing with a deadline-based limiter against a
      fixed 60 Hz clock (mGBA frame-limiter style: overshoot eats the next tick's
      budget instead of accumulating drift; large gaps snap forward so a host
      suspend/resume doesn't fast-forward the game), and making the frontend drop
      stale frames (`requestAnimationFrame` latest-frame coalescing) instead of
      queueing them behind a slow compositor. Emulation stays decoupled from the
      consumer — the broadcast channel drops for lagging receivers, so "fine with
      dropping frames to stay synced" is the built-in contract.
- [x] **Observability** (2026-08-16, commit xxx):
      - Per-second stats line for every instance, streamed to stdout AND the frontend:
        `[t=52s] emu P1=[57.4] P2=[59.4] fps | sleep:[.T] | video:29.7 fps` — frame
        drops are visible the second they happen (`sleep:[T.]` = primary waiting on
        the link). Stall detection warns on-screen and in the log if any instance runs
        under 30 emu fps for 2+ s, and reports recovery.
      - C stdout is unbuffered (`setvbuf(_IONBF)`) so mGBA WARN/ERROR lines hit the log
        file immediately even on a hard crash (C stdout is fully buffered when
        redirected to a file, so lines were previously lost in the buffer).
      - mGBA WARN/ERROR/FATAL lines are forwarded over the WebSocket as text frames
        and rendered as an on-screen debug overlay (bottom ticker, last 6 lines),
        colored by severity. Implemented as a custom `mLogger` callback that formats
        the printf-style message via `vsnprintf` (bindings already expose it).
      - Player color coding like a video call: P1 red, P2 blue, P3 green, P4 orange
        border around each tile + matching bottom-right tag.
      - ROM load auto-starts emulation (`load_rom` sets `is_running`); no extra
        "start" action needed — confirmed from the log (`ROM loaded` → frames flow
        the same second).
- [x] **GBA link-test ROM** (2026-08-17, `DualBoy/linktest/`): a MULTI-mode
      link instrument (v2.0 supports up to 4 linked units; v1.0 was 2-player) that
      shows, in near-real-time, the data each unit sends and receives
      (`SIOMLT_SEND`/`SIOMULTI0-3`, all four slots), the per-slave round-trip time
      (master) / ping cadence (slaves) in frames, current + partner + expected
      state, a STALL counter, a live PEERS count on the master, and a live
      sparkline. Built with bare `clang` + `arm-none-eabi-ld` (no devkitARM
      needed; see `build.sh`). Its `FRM` counter (game frames since boot) is the
      key desync probe — ALL screens must advance in lockstep. ROM bug found while
      building it: RCNT must be cleared (`REG_RCNT = 0`) before MULTI mode or
      mGBA's mode decode (`(rcnt & 0xC000) | (siocnt & 0x3000) >> 12`) reads GPIO
      instead of MULTI (dead link). v2.1 later dropped the per-frame full-screen
      clear (it made the whole instrument crawl) — see the 60 fps entry below.
- [x] **Lockstep desync + 128s crash fix** (2026-08-17): the linktest exposed that
      the primary ran at ~half the secondary's speed (`FRM 130` vs `234`), and the
      link died/spun at ~128 s. Root cause: mGBA's lockstep `user->sleep` is meant
      to BLOCK a player's host thread, but DualBoy runs both players on one thread,
      so the old `++video.frameCounter` early-exit hack split the primary's ROM frame
      across two 60 Hz ticks (half speed) while the secondary — whose sleep flag the
      frame loop ignored — ran ahead until its 32-bit cycle clock wrapped and
      `_untilNextSync` returned a negative delay (infinite re-entry spin / assert).
      Fixed by replacing whole-`runFrame` pacing with cooperative stepping: the frame
      loop advances each player one timing event at a time (`runLoop`) up to a
      280896-cycle frame budget, skipping any player whose sleep flag is set until
      the other player's work wakes it. `GBASIOLockstepPlayerSleep` no longer bumps
      the frame counter (that was the half-speed cause), and `_lockstepEvent` clamps
      a non-positive sync delay to 4 cycles instead of asserting. Verified on the
      linktest: both players hold 60.0 fps with `FRM` in exact parity, STALL 0, and
      no crash past the old 128 s wrap point (264 s+ observed).
- [x] **4-player linktest verified** (2026-08-17): `--players 4` runs four instances;
      the v2.0 linktest shows P1 MASTER + P2/P3/P4 SLAVE (SIOCNT ids 0x1/0x2/0x3,
      slave bit set), all four slots S0-S3 live with each slave's echo, master RTT
      ~2f to every slave, and FRM in exact parity across all four (3100==3100==3100
      ==3100 after ~7 min). All four hold 60.0 fps, zero stalls, no crash — the
      cooperative-stepping fix scales cleanly to the full 4-player lockstep. The
      desktop app also gained P3/P4 keyboard maps (`--players 3|4` is fully
      playable from one keyboard).
- [x] **4-player 60 fps game time — wrapper exonerated, linktest ROM perf fixed**
      (2026-08-17): a minimal-draw build of the linktest (full MULTI link protocol,
      one-line readout) ran all four at exactly 60.0 fps — proving the wrapper/
      lockstep is NOT the bottleneck. The crawl was the ROM's own rendering: it
      cleared all 38,400 framebuffer pixels and redrew ~250 glyphs every frame,
      spilling vblank into mode-3 bitmap VRAM contention (~8 fps). Two ROM fixes:
      (1) draw static labels once at boot and only clear+redraw the live value
      cells (change-cached) each frame; (2) use SIOCNT baud 3 (fastest MULTI clock)
      — at the default baud 0 a 4-player transfer costs ~126k cycles and the
      master's transfer+hard-sync pushed its loop to 2 frames while slaves ran 1
      (a false 30/60 FRM desync). After: all four FRM in exact parity at ~60 fps,
      STALL 0, no stall/crash. Also made the frame broadcast tear-free in
      `emulation.rs` by snapshotting each player's buffer at its frame-counter
      increment (vcount 160, right after `finishFrame`, before the ROM's next-frame
      clear) instead of reading the live mid-frame buffer. `cargo test` green
      (128 unit + 2 smoke).

In progress / next:
- [ ] Root-cause the one observed tokio-worker segfault (`segfault at 4a8` in
      `dualboy-web`): suspected cross-thread `load_rom` (tokio) vs `run_frame` (std
      emulation thread), possibly aggravated by the old debug build's log flood. Needs
      sustained-play re-testing now that logging and build profile are fixed. The new
      per-second stats + unbuffered stdout make a recurrence visible immediately in
      `/tmp/dualboy_app.log` before any crash.
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
cargo run --release --bin dualboy-web -- --players 4            # http://127.0.0.1:8080 (video 60)
cargo run --release --bin dualboy-web -- --players 4 --fps 30   # headless: throttle video to 30
cargo run --release --bin bench -- <rom.gba> [players]         # emulation speed only
```

Build notes: first `libmgba` build takes minutes and several GB RAM (needs cmake + clang).
This workspace has 15 GB RAM / 8 cores, which is plenty. `--fps` sets the video send rate
(1–60, default 60: every emulated frame) independently of emulation speed; lowering it
never slows emulation. Always use `cargo build --release`.

## Handoff notes for future sessions

- Read this file + `DualBoy/src-tauri/src/*.rs` and `DualBoy/src/main.js`; the rest of
  the repo is upstream mGBA.
- **Troubleshoot link/perf issues with the linktest ROM FIRST.** When anything smells
  like a lockstep/desync/performance bug — a game crawling on a link-heavy screen
  (e.g. Four Swords title select), "MULTI did not receive data", the multi-pak
  power-off/on screen, or FRM counters diverging — load
  `DualBoy/linktest/linktest.gba` (rebuild with `./build.sh` if you touched it) and
  watch the on-screen readout on every player before touching game-specific tuning:
  roles (MASTER/SLAVE), all four slots S0-S3, per-slave RTT, STALL, PEERS, and —
  most importantly — that all FRM counters advance at the same rate. Run with
  `--players 2|3|4` to reproduce at the same scale as the failing scenario. This ROM
  is a link-heavy MULTI-mode game by design, so any wrapper-level frame starvation
  shows up here as an FRM divergence or a rising STALL, and it is far easier to
  reason about than a real game. (See `DualBoy/linktest/main.c` header for the full
  protocol + expected readouts.) Two linktest details that matter: keep SIOCNT baud
  at 3 (fastest) — the default baud 0 makes the master's loop run 2 frames and shows
  a false FRM desync — and don't re-add a per-frame full-screen clear (draw static
  once, change-cache the live values); both were found to masquerade as wrapper
  perf bugs.
- Commit + push frequently (history has been lost to VM OOM before).
- Follow YAGNI; prefer small, single-purpose changes.
