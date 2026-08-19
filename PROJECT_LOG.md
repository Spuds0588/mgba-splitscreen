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

## Player-count selector (1–4) + real-game linked test

- **UI:** new **Players** menu in the top bar lets you pick 1–4 linked instances
  (solo = single GBA with no link driver). The menu highlights the active count, and
  changing it restarts the emulator with that many instances. Backend: `set_player_count`
  command on `lib.rs`; the emulation manager now stops/rebuilds its instance set at
  runtime instead of taking the count only from the `--players` CLI flag (still honored
  as the initial default). Works in both the Tauri app and `dualboy-web`.
- **P3/P4 keyboard maps** were added earlier for 4P (`DualBoy/src/main.js`), disjoint
  from P1/P2.
- **Real-game linked test (2P, Four Swords):** drove both players through the full
  flow — boot → name entry (AAAA) → save → file select → CHOOSE A GAME → Four Swords
  → FS title → character select → in-game story/pause screens — entirely over the WS
  with `nav_fs.py` + manual button pushes. The character-select screen that used to
  crawl now runs at full speed. Both players held **60.0 fps for 672+ seconds** with
  zero stalls, and per-player inputs are independent and responsive (P1-only START
  changed only P1's frame). The earlier "stuck at title"/"alttp" readings were OCR
  misdetections of FS's bright title/character-select screens, not a hang.

## Four Swords link handshake — root cause (2026-08-17, still OPEN upstream)

The FS linking screen ("Linking with other systems… Please wait a moment") does
**not** establish the link — it hangs there. This is a real bug, not a nav
misdetection. Root-caused as far as practical with player-tagged SIO tracing
(value-change-gated reads/writes of SIOCNT/SIOMLT_SEND/SIOMULTI + the lockstep
MODE_SET/TRANSFER_START/READY/hard-sync event flow):

- **The data path is correct.** Both players reach MULTI mode, correct IDs
  (P0=parent `200B`/id 0, P1=child `601F`/id 1), ready bit (SD) = 1. 12,741
  transfers complete, and every `MULTI transfer finished` value matches exactly
  what each side wrote to `SIOMLT_SEND` (verified: `FEFE 0000` → `0068 006A` →
  `FF89 0000` → `0000 0000`…). No corruption, no 4-bit-vs-16-bit issue — GBATEK
  confirms MULTI sends the full 16-bit SIOMLT_SEND per player.
- **The game still retries forever.** The handshake is `FEFE` probe → value →
  checksum (value + checksum = 0xFFF1, a validity pair) → idle `0000`s → next
  `FEFE`. 984 `FEFE` probes in one capture = 984 failed attempts. In one attempt
  both sides sent IDENTICAL value+checksum (`006A`/`FF87`) and it still reset, so
  it is not a value mismatch — the master/slave state machines desynchronize.
- **The game reads SIOCNT only** (confirmed zero `RCNT` reads), so the failure is
  in a SIOCNT bit it polls (busy/ready/ID) or in the timing of transfer completion
  relative to the game's poll — not in an unmodeled RCNT SC bit.
- **Our C SIO code is upstream-identical.** `diff` vs `mgba-emu/mgba` master shows
  `sio.c`/`io.c` byte-identical and `lockstep.c` differing only by the committed
  non-positive-delay clamp (for the 128s wrap) — so this is **upstream mGBA
  issue #3286** ("can't get past the Linking screen", still open, `blocked: needs
  retest`), not a DualBoy wrapper regression. It reproduces in stock mGBA.
- **Most promising lead:** upstream's `a0647ffac` "Loosen timing where possible"
  (UNLOCKED_INTERVAL 4096→8192; delay hard sync while `waiting`; reset
  `nextHardSync` in `AckPlayer`) was an attempt at exactly this kind of handshake
  race and was REVERTED (`ea50b5e87`). Re-applying it (or otherwise thinning the
  hard-sync cadence during a transfer burst) is the next thing to try, but do it
  as a git branch and re-verify the linktest FRM parity + the 128s crash first —
  the revert was intentional upstream.

## Branch `fs-link-loosen-timing`: a0647ffac re-applied + tested (2026-08-17)

Created branch `fs-link-loosen-timing` from master and cherry-picked upstream's
`a0647ffac` "GBA SIO: Loosen timing where possible" (UNLOCKED_INTERVAL 4096→8192;
hard sync only when `!waiting`; `nextHardSync` reset to HARD_SYNC_INTERVAL in
`AckPlayer`). Tested both the 4-player linktest FRM parity and a real Four Swords
2-player link session together:

**4-player linktest FRM parity — PASS (no regressions).** All four instances held
60.0 fps (emu + video) for 214s+; FRM counters climbed in exact parity
(+3,285 then +6,364/+6,364/+6,364/+6,365 in identical windows); STALL=0 on all
four; zero WARN / "did not receive" lines; no 128s crash. Slaves actively wrote
`SIOMLT_SEND` (echo pings), so the transfer path is healthy. Re-run:
`python3 /tmp/linktest_parity.py 4 <label>` with the app at `--players 4` and
`linktest.gba` loaded.

**Four Swords 2P linking — MATERIALLY IMPROVED but still not playable.** With both
players pressing START simultaneously at the FS title, the game now passes the
linking screen into the character-select (4-slot screen) — the exact state that
hung forever pre-patch (0 transfer starts). 177,477 transfers flowed with both
players exchanging value+checksum pairs (e.g. `0044`/`FFAD`, `001D`/`FFD4`), and
both advanced through char-select together at 60 fps. The game then **stalls on the
post-link screen** (FOUR SWORDS logo banner + text menu): screen freezes (0 px
animation), transfers stop mid-probe (P1 writes `FEFE` but the transfer-start
SIOCNT write never follows — the game is waiting to receive before sending again),
and only B responds (backs out to the linking screen). The stall is in the game's
SIO state machine, i.e. the tail of upstream #3286 — the wrapper delivers transfers
correctly (linktest proves it).

**Verdict:** the patch is safe (linktest unaffected) and is the best-known state for
FS linking (the handshake completes instead of hanging forever), so keep it on the
branch. It is not a complete fix; the remaining post-link stall needs the deeper
handshake-timing work (or an upstream fix) — see the root-cause section above.
`emulation.rs` currently has a TEMP `mLogFilterSet("gba.sio", DEBUG)` enable to
make the handshake/transfer event flow visible in `/tmp/dualboy_app.log`; remove it
when done debugging.

Next time: reproduce with the linktest ROM first (see handoff notes) to confirm
lockstep health, then attack the post-link stall — the game stops initiating
transfers on a screen where it must exchange data to proceed.

## FS post-link stall — working hypothesis list (2026-08-18)

Ordered by priority; each entry records what to try, why, and how to tell if it
worked. Test ONE at a time, rebuild, reproduce, and log the result here so we
don't backtrack. (Key upstream context: lockstep.c on mgba master is still
identical to ours modulo the committed clamp; issue #3286 is still OPEN; the
`a0647ffac` "Loosen timing" patch is a Feb 2026 upstream experiment that was
reverted three weeks later — so there is no upstream fix to cherry-pick yet.)

- **H1 — Per-transfer hard sync is too aggressive.** `finishMultiplayer/8/32`
  calls `_hardSync()` after EVERY completed transfer, a full barrier (primary
  sleeps until all secondaries ack). Real hardware has no such per-transfer
  barrier — the master clocks the transfer and everyone finishes together.
  FS's rapid FEFE→value→checksum handshake may need the games to run a few
  cycles apart, and this barrier pins them too tightly / at the wrong cadence.
  *Try:* gate or remove the per-transfer `_hardSync` (leave the periodic
  `nextHardSync` one). *Pass:* FS gets past the post-link screen and into
  story/gameplay; linktest FRM still in parity, STALL 0, no 128s crash.

- **H2 — Mode-switch negotiation race.** Switching mode (FS cycles GPIO→NORMAL8
  →MULTI during detection) makes the primary `WaitOnPlayers` + sleep until all
  ack. If MODE_SET events and acks interleave badly, `transferMode`/`otherModes`
  can be momentarily inconsistent and the SD (ready) bit flickers, so the game
  never sees a stable "all ready". *Try:* instrument mode-set + SD transitions;
  if SD toggles spuriously, fix the ordering. *Pass:* SD stable; FS links.

- **H3 — Sequential wrapper timing ≠ threaded-model timing.** mGBA's lockstep is
  designed for the threaded model (each player's `user->sleep` BLOCKS its host
  thread; a separate mCoreThread frame-sync paces frames). DualBoy runs all
  players on one thread via cooperative `runLoop` stepping. FS is timing
  sensitive where the linktest ROM is not. *Try:* build a small threaded C
  harness against libmgba (2 cores, 2 mCoreThreads, real blocking lockstep
  users) and run FS. *Pass/fail discriminates:* links there = our wrapper
  adaptation is at fault; still stalls = upstream protocol bug.

- **H4 — Ready/ID/SI bit semantics differ from hardware for FS.** `_setReady`
  computes SD = (all players' modes match). FS may poll SI (bit 2), the ID bits,
  or SD at moments where mGBA's lockstep reports a different value than a real
  link port would (especially before/after mode negotiation settles). *Try:*
  dump the exact SIOCNT values FS reads vs gbatek's documented MULTI-mode
  layout; fix any divergence. *Pass:* game stops retrying and links.

- **H5 — The post-link "stall" is a nav/input false positive.** The logo+text
  screen after linking might legitimately await a specific input (both players
  press START, a menu choice, etc.) and only LOOKS frozen. *Try:* systematic
  button sweeps on both players there. *Pass:* a button advances it; no SIO fix
  needed. (Prior evidence against: 0-px animation, transfers stop mid-probe,
  only B responds — but cheap to rule out definitively.)

- **H6 — SIO IRQ / interrupt timing.** FS may use the SIO IRQ (SIOCNT bit 14) or
  depend on interrupt latency for handshake timing, which lockstep delivers at
  slightly different points than hardware. *Try:* compare IRQ fire times with
  transfer completion; adjust if off.

- **H7 — A fix exists on an mGBA PR/branch not yet merged.** *Try:* search
  mGBA's open PRs + forks for a #3286 fix before writing our own.

Status log (append as each is tested):
- [x] H1 — per-transfer hard sync removed from `finishMultiplayer`:
      **linktest PASS** (exact FRM parity +596/+596, 0 data-loss, 60 fps both),
      **FS handshake desync REDUCED** (off-by-one + missed rounds mostly gone;
      after a one-time hiccup the games exchange MATCHING value+checksum pairs
      in lockstep) — but the handshake still cycles forever, so H1 is a partial
      improvement, NOT the complete fix. Kept on the branch as a cumulative
      experiment. This tells us the completion failure is NOT the off-by-one
      desync — with clean lockstep sync the game still doesn't accept the link,
      pointing at a register-bit or transfer-timing mismatch (H4/H6) next.
- [ ] H2
- [x] H3 — **tested + resolved (2026-08-18)**: built `DualBoy/tools/threaded_link.c`
      (real threads, real blocking deferred sleep/wake, 60fps pacing, ported nav_fs
      screen detection) and drove FS into the link screen. 95,056 transfers, 0
      drops, but 7,341 FEFE probes at ~120/s for the whole 60s → the handshake never
      completes, identical to the sequential wrapper. **Threaded execution does NOT
      fix FS; the bug is in `lockstep.c` (upstream #3286).** Do NOT spend effort
      porting DualBoy to threads as a link fix — it is ruled out.
- [ ] H4
- [ ] H5
- [ ] H6
- [ ] H7

### Fresh reproduction + handshake data (2026-08-18)

Reproduced reliably via `dualboy-web --players 2` + `nav_fs.py --port 8080
--path /ws` (drives both to the FS title) + a simultaneous START press on both.
Result: 52,811 transfers flow, 0 "did not receive", and the full MULTI handshake
is visible in the `MULTI transfer finished` log. The handshake is a repeating
cycle, and the raw slot data shows the desync directly:

```
FEFE 0000   P1 probes, P2 idle
0042 0043   P1=0x42, P2=0x43   <-- P2 one round AHEAD
FFAF FFAE   0x42+0xFFAF=0xFFF1, 0x43+0xFFAE=0xFFF1 (checksums)
0000 0000 x ~20 idle transfers
FEFE 0000
0043 0000   P2 missed this round (idle)
FFAE 0000
0000 0000 x ~20
FEFE 0000
0044 0044   both in sync
FFAD FFAD
...
```

The value increments each round (0x42→0x43→0x44→…), and value+checksum always
sums to 0xFFF1 (a validity pair). Two failure signatures recur: (a) P2 is one
value AHEAD of P1 (`0042 0043`), and (b) P2 misses a round entirely (`0043 0000`).
258 distinct rounds were observed with the games never completing the handshake
— they are stuck exchanging incrementing values, i.e. the master/slave state
machines keep drifting apart and resyncing. This is the concrete manifestation
of the off-by-one timing skew that the earlier session could only infer.

Reproduction recipe (save this):
1. `cd DualBoy/src-tauri && ./target/release/dualboy-web --players 2 > /tmp/dualboy_web.log 2>&1 &`
2. `curl -X POST --data-binary @"Test Roms/...Four Swords....gba" http://127.0.0.1:8080/load_rom`
3. `python3 DualBoy/scripts/nav_fs.py --port 8080 --path /ws --players 2 --rom "Test Roms/..." `
   (drives both to the FS title)
4. Press START on both simultaneously (raw_ws keys START on players 1+2).
5. Watch `/tmp/dualboy_web.log` for `MULTI transfer finished` values.

## Future dev options (documented, not yet built)

- **2×2 link groups** — two independent 2-player links (P1–P2, P3–P4) in one process,
  or arbitrary combinations of linked/independent cores, so users can run two separate
  pairs or the same game independently without linking.
- **Per-player screen toggle** — show one player's screen at a time (switchable), so
  RetroAchievements/netplay-style play where each player watches their own screen is
  possible before any RetroArch port.
- **Pop-out windows** — spawn each player's screen as its own OS window for multi-screen
  setups or custom streamer layouts.

In progress / next:
- [ ] Root-cause the one observed tokio-worker segfault (`segfault at 4a8` in
      `dualboy-web`): suspected cross-thread `load_rom` (tokio) vs `run_frame` (std
      emulation thread), possibly aggravated by the old debug build's log flood. Needs
      sustained-play re-testing now that logging and build profile are fixed. The new
      per-second stats + unbuffered stdout make a recurrence visible immediately in
      `/tmp/dualboy_app.log` before any crash.
- [ ] Drive Four Swords to *actual gameplay* (past the story intro) at 4P and confirm
      the link-heavy title select stays smooth there too.
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

## Root-cause direction for the Four Swords link stall (2026-08-18)

**The wrapper deviates from mGBA's link design.** mGBA's multiplayer path expects
EACH core on its OWN `mCoreThread` (a real OS thread running `core->runLoop`),
with `mLockstepThreadUser` wiring `sleep`/`wake` to `mCoreThreadWaitFromThread` /
`mCoreThreadStopWaiting` (real thread blocking/resume). The Qt "new multiplayer
window" feature is built exactly this way (`src/platform/qt/MultiplayerController.cpp`
+ `src/core/thread.c` + `src/core/lockstep.c`).

DualBoy instead steps every instance SEQUENTIALLY on one thread and fakes
`sleep`/`wake` with per-player flags (`emulation.rs` `LockstepUserCtx`). That flag
reimplementation is a re-do of a subtle synchronization protocol, and it is the
prime suspect for the residual FS stall *on top of* the known upstream issue
[mgba#3286](https://github.com/mgba-emu/mgba/issues/3286) (which even threaded
mGBA 0.10.3 hit, so threading alone may not be sufficient).

**Decisive experiment built:** `DualBoy/tools/threaded_link.c` + `build.sh` — a
headless harness that runs N cores on N real threads through upstream's own
`GBASIOLockstepCoordinator` + `GBASIOLockstepDriver`. The per-player thread loop
replicates `mCoreThread`'s semantics exactly: lockstep `sleep`/`wake` are deferred
to the loop (`sleep` sets a flag and returns — it runs under the coordinator mutex
and MUST NOT block — and the loop blocks on a condvar after `runLoop` returns), and
each thread paces itself to ~60 FPS. `--fs <rom>` drives both players through the
nav_fs screen-detection state machine (ported to C) into the link screen and dumps
PPM frames to `/tmp/fs_*.ppm`.
Build with `DualBoy/tools/build.sh` (it re-derives the cmake `-D` defines so struct
layouts match `libmgba.a`; must define `-DUSE_PTHREADS -DENABLE_VFS -DENABLE_DIRECTORIES`
or structs/`mCoreLoadFile` are mis-sized/undeclared). Usage:

    DualBoy/tools/threaded_link <rom.gba> <2-4> <seconds> [script.txt]

`script.txt` lines are `time_ms player keymask` (A=0x1 B=0x2 Sel=0x4 Start=0x8
R=0x10 L=0x20 U=0x40 D=0x80 R-trig=0x100 L-trig=0x200). Judge the link from stdout
SIO DEBUG chatter: `Transfer starting`, `MULTI transfer finished`, and `did not receive`.

**Validated:** 4-player linktest, 8 s → 9495 `Transfer starting`, 37980 `MULTI transfer
finished` (= 4 players × 9495), ZERO `did not receive`, zero aborts/desyncs. The
harness faithfully reproduces mGBA threaded lockstep.

**RESULT — the split test is DONE (2026-08-18):** `threaded_link --fs` drove both
players through name entry → save → game select → Four Swords → FS title, then
pressed START on both simultaneously and watched the link for 60s:

- **95,056 transfers started, ZERO "did not receive", zero aborts/desyncs.** The
  link DATA path is perfect under threaded execution — same as the wrapper.
- **7,341 `FEFE` probes at a steady ~120/s for the entire 60s** (constant across
  six 10s windows: 1201/1229/1226/1230/1228/1227). The handshake never completes;
  it keeps cycling FEFE → value/checksum → idle, and both players' screens freeze
  on the same post-title screen (PPM dumps: pre-START title ≠ post-START screen,
  which is then pixel-stable for 50s).

**Conclusion: threaded execution (mGBA's own canonical local-multiplayer model) does
NOT fix Four Swords — it stalls at the same handshake the wrapper stalls at.** The
bug is in `lockstep.c` (upstream #3286), not in DualBoy's single-threaded wrapper.
This RETIRES the "port DualBoy to mCoreThread" idea as a fix (it's still useful
later for the pop-out-windows feature, but it won't fix linking).

**What to do instead (fix the DRIVER):**
1. **H4/H6** — instrument the exact SIOCNT/SIOMULTI bits + transfer-completion/
   IRQ timing FS polls during the FEFE cycle, and diff against gbatek's MULTI
   semantics. The handshake now fails with *matching* data in clean lockstep, so
   it's a register-bit or completion-timing mismatch, not a data race.
2. **Bespoke deterministic in-process `GBASIODriver`** — the "rewrite the link
   logic" option. Instead of mGBA's free-running + periodic-hard-sync lockstep
   (built for network netplay jitter), write a driver for the in-process case that
   rendezvouses every MULTI transfer at a single shared cycle: on master START,
   read all players' `SIOMLT_SEND`, schedule each core's completion event at
   `current_cycle + transferCycles` simultaneously, no hard-sync drift. This
   removes the entire class of timing skew FS trips over. Regression-gate it with
   the 4-player linktest FRM parity + the 128s wrap test.
3. Keep watching upstream #3286 (still `blocked: needs retest`) and the reverted
   `a0647ffac`/`ea50b5e87` timing experiments for a merged fix.

## Bespoke deterministic in-process driver (`GBASIORendezvousDriver`) — 2026-08-18

Built the "rewrite the link logic" option as `DualBoy/tools/rendezvous.{c,h}` +
wired it into `DualBoy/tools/threaded_link.c` as a selectable in-process driver.
It is a fork of `lockstep.c` with two changes: (1) no per-transfer `_hardSync`
(H1) and (2) no post-ack secondary sleep in `AckPlayer` — the secondary runs on
to `finishCycle` so both sides complete at the same shared cycle. The harness
keeps real threads + 60fps pacing + the nav_fs state machine, and now dumps a
per-transfer drift measure (`Rendezvous: player N time X finishCycle Y drift D`).

**Regression — 4-player linktest: PASS, and cycle-lockstep is now measurable.**
Every secondary processes every TRANSFER_START within ~30 cycles of the master
(drift distribution is a tight spike at the transfer time: e.g. `drift -5755`
for FS's baud, meaning `slave_time ≈ master_time`). 0 "did not receive", 0
aborts. The deterministic rendezvous does exactly what it was designed to do —
it removes cycle drift as a variable.

**Four Swords: STILL fails, and now we know why it is NOT cycle drift.** With
both cores pinned to the same cycle, the game handshake still never completes:
`FEFE` probes continue at ~120/s and the games exchange value+checksum pairs
whose counters are offset by 13→19 and keep drifting. **The failure survives
tight cycle-lockstep**, so it is a register-bit or transfer-completion-timing
mismatch (H4/H6), not a data race and not frame/cycle drift.

**New ground-truth instrumentation (temporary, now reverted — re-add to
`src/gba/io.c` `GBAIORead` when needed):** value-change-gated SIOCNT read trace
(`mLOG(GBA_SIO, DEBUG, "SIOCNT read P%d: %04X busy=%d ready=%d err=%d id=%d slave=%d", ...)`)
plus the SIOCNT/SIOMLT_SEND write logs already in sio.c. The decisive asymmetry
it exposed during the FS link screen:

- **Master P0** writes SIOCNT `208B` (baud 3, busy=1) **94,614 times** — it
  settles at baud 3 and drives every transfer. It reads `busy=1` ~50% of the time.
- **Slave P1** writes SIOCNT `601F`/`201C` (baud 3 / baud 0, **busy=0 always**)
  only 184 times — it never sets busy, it alternates baud 0↔3 forever, and it
  reads `busy=1` only **5 times out of 194** reads (2.6%) even though the
  master's transfers hold the slave's busy bit high ~52% of the time.

So the two games are in **different handshake states**: the master is probing
(FEFE + busy), while the slave is stuck in baud negotiation and almost never
observes a transfer in progress. That is the concrete manifestation of the
value-counter offset. Data exchange itself is perfect (189,228 =
2×94,614 `MULTI transfer finished`, zero `did not receive`).

**Hypothesis tested this round (NEGATIVE):** simulate MULTI baud-mismatch
corruption (slave at a different baud than the master reads all-ones instead of
clean data, since real hardware clocks the wire off the master's SC). The
corruption branch never fired — `Baud mismatch` count was 0 across the whole run
because the slave's baud is always 3 at every transfer *completion* — so the
slave's baud-0 writes only happen outside the transfer burst and this is NOT the
bug (reverted).

**Best next move:** the slave reads `busy=1` ~5 times when a faithful model
should show it ~50% of the time. Either the slave's busy bit is being cleared
before the game's (rare, ~3/s) SIOCNT read, or the game never polls busy during
a transfer. Instrument the slave's busy-bit set/clear cycle boundaries in
`finishMultiplayer`/the TRANSFER_START handler vs the exact cycle the game reads
SIOCNT, and check whether mGBA clears busy too early relative to gbatek's
"busy stays high for the full transfer" semantics. This is the narrowest lead we
have and it is fully reproducible via the harness (`threaded_link --fs <rom>`,
then grep the stdout).

## Busy-bit boundary instrumentation — DONE, hypothesis REFUTED (2026-08-18)

Instrumented the rendezvous driver (`DualBoy/tools/rendezvous.c`) + `src/gba/io.c`
`GBAIORead` and re-ran `threaded_link --fs <rom>` (94,601 transfers, 0 drops):

- `BUSYSET pid=1` logs the slave's busy-set cycle + `nextEvent`; `BUSYCLR pid=1`
  logs its clear cycle; `BUSYRD` logs the game's SIOCNT reads (value-change gated
  on the busy bit). Rebuild: `cmake --build <cargo out>/build && DualBoy/tools/build.sh`.

**Result — the slave's busy bit is NOT cleared early.**

- Window `clear − set`: min 5,659 / **median 5,755** / max 13,683 cycles across
  all 94,601 transfers. The median is exactly `GBASIOCyclesPerTransfer[baud 3][1]
  = 5,755`, i.e. the full nominal transfer. **0 / 94,601 windows were < 5,000
  cycles** — the "clears busy too early" hypothesis is refuted.
- The game's reads line up correctly with the window when it does poll: it reads
  `busy=1` ~2,000 cycles after set (well inside the 5,755-cycle window) and
  `busy=0` ~130 cycles after clear. The busy bit is fully observable.

**Sharper finding this exposed (the new lead):**

- **Master (pid 0) reads SIOCNT 7,635 times** over the 60 s handshake (3,817
  busy 0→1 + 3,817 1→0 transitions).
- **Slave (pid 1) reads SIOCNT only 11 times** — 5 `busy=1`, 5 `busy=0`, 1 boot
  read. Its reads are clustered in short bursts (gap ~3,876 cycles within a burst)
  separated by huge gaps (median 277 k cycles ≈ 16.5 ms, one ~59 s).

So the slave's game is **not** in a busy-poll loop at all — it is writing
`SIOMLT_SEND` (86,904 times) and cycling SIOCNT baud 0↔3 (`201C`↔`601F`, IRQ bit
14 = 1), then waiting *long* stretches between handshake attempts. The busy bit
is fine; what the slave is waiting on is the real question — most likely the
**SIO completion IRQ (SIOCNT bit 14, which the slave has set)** or an
interrupt-driven wait, i.e. **H6**. Next: trace `GBARaiseIRQ(GBA_IRQ_SIO)` fire
cycles vs transfer completion on the slave, and whether the slave's BIOS
`IntrWait`/halt is being released.

**NOTE — temporary instrumentation currently IN the tree** (uncommitted):
`src/gba/io.c` (`g_busyTraceLast/g_busyTraceInit` + the `BUSYRD` mLOG in
`GBAIORead`), `src/gba/gba.c` (SIOIRQ/TRIG/HALT trace in `GBARaiseIRQ`,
`_triggerIRQ`, `GBAHalt`), `DualBoy/tools/rendezvous.c` (`BUSYSET`/`BUSYCLR`
mLOGs). Revert `src/gba/io.c` and `src/gba/gba.c` before any merge; the
`BUSYSET`/`BUSYCLR` driver logs are cheap and can stay in the dev tool.

## SIO IRQ fire vs transfer completion — DONE, H6 REFUTED (2026-08-18)

Task: trace `GBARaiseIRQ(GBA_IRQ_SIO)` fire cycles vs transfer completion on the
Four Swords slave, and check whether the slave's BIOS `IntrWait`/halt is being
released during the handshake. Added value-gated SIO IRQ / halt trace to
`src/gba/gba.c` (SIOIRQ in `GBARaiseIRQ`, TRIG in `_triggerIRQ` incl. `wasHalted`
+ IF/IE/IME snapshot, HALT in `GBAHalt`), rebuilt libmgba + harness, re-ran
`threaded_link --fs <rom>` (94,599 transfers, 0 drops).

**Result — the slave's SIO IRQ fires at exact transfer completion and its halt
IS released, every time. H6 refuted.**

- SIO IRQ raise vs BUSYCLR (transfer completion): **delta min 0, p50 0** cycles
  across 64,238 raises — the IF bit is set in the same timing tick the transfer
  finishes.
- Halt release: 55,146 `TRIG` events with `if=0080 wasHalted=1`, and the IRQ
  trigger fires **exactly GBA_IRQ_DELAY (7) cycles** after transfer completion
  (p50 7). The slave's `IntrWait` halt is released by the SIO IRQ on every
  transfer — the interrupt delivery path is flawless.
- The slave IS in `IntrWait` during the handshake (only 1 `HALT` *state-change*
  line because the IE/IF/IME snapshot is stable: `ie=2085 if=0000 ime=1`, which
  has the SIO bit 0x80 enabled in IE — the gating logs state changes, not count;
  the wake counter proves repeated halts+wakes, 242,621 total wakes).
- Transfer cadence on the slave: p50 19,210 cycles ≈ 833 transfers/s.
- The handshake value/checksum pairs are **internally valid both ways**
  (`0088+FF69=FFF1`, `0095+FF5C=FFF1`) — each game computes `checksum =
  0xFFF1 − value`, and the receiving side sees a consistent pair. Data integrity
  is perfect; the games still never accept the link.

**Conclusion:** the interrupt/halt path (H6) is NOT the bug. Combined with the
busy-bit result (H4 refuted) and the cycle-lockstep result (H1/desync refuted),
all four "plumbing" hypotheses are now dead. The remaining lead is the value
counter itself: the two games' handshake counters stay offset (~0x13, P1
advancing faster in bursts) even under perfect cycle-lockstep and valid
checksums, so the game's acceptance check sees a value it never accepts. Next
narrowest probe: trace the slave's SIOMULTI0-3 *reads* and its branch on the
received value during the FEFE cycle — i.e. what value does the game expect to
receive, and what does the driver actually deliver at that exact read cycle?

## Ordered path forward (priority / likelihood) — 2026-08-18

Working top-down; each is designed to either produce the fix or kill a whole
branch cheaply. Record result after each; do not re-derive dead branches.

- **P1 — Trace the slave's SIOMULTI0-3 reads + response branch.** The one
  remaining unknown: what value does the game read back and how does it branch
  on it? Correlate each slave SIOMULTI read (cycle, value, slot) with the
  transfer that produced it and the write that follows. If the game reads slot
  data that never matches an expected value (or reads at the wrong cycle), this
  pinpoints it. Highest information-per-cost.
- **P2 — Scope test: Shining Soul II / SMA4 2P.** Both have working 2P link.
  If they link where FS stalls → driver is fine for MULTI, FS protocol needs
  something specific. If they stall identically → driver-wide issue (slots,
  latching, cadence) and the fix benefits everything. Cheap, high
  discrimination.
- **P3 — Verify SIOMULTI read-back semantics vs gbatek.** Slot mapping
  (SIOMULTI0=master's data, SIOMULTI1=slave1's...), reads during a busy
  transfer (stale vs live), and whether the driver latches data at the same
  cycle real hardware does. Pure spec check — no game needed.
- **P4 — Value-convergence experiment.** If P1 shows the games need matching
  (value, checksum) pairs, force/diagnose convergence: do the games resync if
  the master echoes the slave's value, or if both start from the same seed?
  Determines whether the driver can nudge the protocol.
- **P5 — Driver fix + regression.** Apply the winning hypothesis to the
  rendezvous driver, then gate against 4-player linktest (FRM parity) AND FS
  (link completes) before considering it done.

## SS2 scope test — Tauri desktop app, live run (2026-08-18, ~22:20)

Ran "Shining Soul II (U).gba" in the actual Tauri desktop build (2 instances,
lockstep driver attached at boot). User drove it manually to gameplay.

**Result: NO crash — session holds.** At t=399s: process alive, 0 WARN/ERROR/
panic lines in a 268 MB log, both players locked at 60.0 fps (emu + video)
for the entire window, transfers continuous.

**Exchange structure (healthy):** value+complement pairs in MULTI mode with
constant 0xB0A6 (e.g. 00EE↔AFB8, B019↔008D both sum to 0xB0A6) — the same
internally-valid checksum scheme FS uses with 0xFFF1, but here the counters
advance in lockstep instead of drifting offset by 0x13. Later payloads show
real game data (7A7A, D325, 8001, 051D) rather than pure FEFE probe/idle,
consistent with in-game link use.

**Conclusion for the scope question:** the link architecture (lockstep driver +
wrapper stepping) is fine for a second commercial game. Four Swords is looking
like an isolated game-specific protocol issue, NOT a driver-wide bug. This
narrows the remaining work to FS's own acceptance/probe logic (the recv==13 vs
cap-at-12 contradiction) rather than a rewrite of the link plumbing.

## Turbo / fast-forward mode (2026-08-18)

User requested a fast-forward before re-testing Four Swords (character-creation
animations are unbearable to watch at 1x). Added a turbo toggle:

- **Backend** (`emulation.rs`): `EmulationManager.turbo: Arc<AtomicBool>` +
  `set_turbo`/`turbo_enabled`. The frame loop skips its 60 Hz pacing sleep when
  turbo is on (lockstep sleeps/wakes still gate linked players, so they stay in
  sync at speed); video broadcast is throttled to ~100 fps so the WS relay isn't
  saturated; an unloaded (no ROM) session idles briefly instead of hot-spinning.
  `set_turbo` prints `TURBO ON/OFF` to stdout+overlay.
- **Commands**: Tauri `set_turbo`/`turbo_enabled` (lib.rs); WS `ClientCommand::
  Turbo` handled in both the desktop WS server (lib.rs) and `web_server.rs`.
- **Frontend** (`main.js`, `index.html`): Tab toggles turbo (routed before
  player keys, never sent to the game); new Speed menu with a Turbo button;
  button highlight + status line reflect state.
- Verified: `cargo build --release` clean, smoke tests pass, app relaunched
  with the new binary.

Next: use Tab to fast-forward FS character creation, then re-evaluate the link
handshake in the fresh `/tmp/dualboy_app.log`.

## Mario Kart Super Circuit 4P + quit-game + audio (2026-08-18, ~23:30)

- **MKSC 4P log**: 72,144 transfers, 0 drops/desyncs, all four at 60 fps for
  298s. "Touchy" linking is the game's boot-time progressive join (slots FFFF ->
  active, visible in the early payloads + sleep:[.TTT] markers), not driver
  errors. Same class as SMW2's detection phase. Driver-side "keep-alive" help
  remains the upstream lockstep alignment experiment (restore per-transfer hard
  sync) — not yet applied.
- **Quit Game**: File -> Quit Game (Tauri `quit_game` + WS `quit_game`) swaps in
  a fresh EmulationManager at the same player count with no ROM; frontend clears
  screens. Keeps the app running for a new load.
- **Audio**: core outputs ONE mixed stereo stream per instance (32768 Hz s16,
  always written to the psg ring — verified `_sample()` writes unconditionally).
  The core CANNOT separate music from SFX (that's game-side channel usage, mixed
  before it reaches mCore), so the menu selects WHOSE mix you hear (P1 default,
  P2-P4, Mix-all with saturating sum, Mute) rather than music-vs-SFX. Capture:
  `setAudioBufferSize(2048)` per instance + `mAudioBufferRead` drain each tick
  (gba.rs `drain_audio`); routing lives in the frame loop (AUDIO_SOURCE atomic,
  skipped in turbo); playback = `src/audio.rs` dlopen of libasound.so.2
  (snd_pcm_open "default" / set_params 32768 stereo / writei) on a dedicated
  thread, bounded 256-msg channel with try_send so a slow device never stalls
  emulation. Validated the ALSA call sequence opens and configures on this
  machine. Non-Linux silently no-ops (video-first).
