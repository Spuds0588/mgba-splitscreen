# agents.md — Orientation for agent sessions

> **Read this first, then `to-do.md` and `history.md`. `PROJECT_LOG.md` has the
> full chronological detail; this file is the fast-start version.**

## What this project is

**mgba-splitscreen** is a split-screen GBA emulator: 2–4 GBA instances running side by
side, synchronized over a virtual link cable (mGBA's lockstep SIO support), so
multiple players can trade/battle/co-op in GBA games on one machine. It is a
fork of **mGBA** (`Spuds0588/mgba-splitscreen.git`, tracking `mgba-emu/mgba`
upstream). The `mgba-splitscreen/` directory is a Tauri v2 desktop app (plus a web
server build) on top of `libmgba`; everything outside `mgba-splitscreen/` is upstream
mGBA source that `libmgba` compiles.

The link-cable handshake bug that this branch was opened for is **fixed** (see
`history.md`: the `_hardSync` experiment reverted), so `fs-link-loosen-timing`
is now waiting to be merged. Treat it as the main line.

**Naming:** the product, the app bundle, the binaries and every path are
`mgba-splitscreen`. The GitHub repo is `Spuds0588/mgba-splitscreen` (it was never
renamed, and does not need to be). Older notes and the on-disk save-state
magics still say "DualBoy"/`DUALSTATE` — those are historical or binary-format
compatibility, not live branding.

## Where things live

- `mgba-splitscreen/src-tauri/src/gba.rs` — `GbaInstance`: wraps one `mCore` (init,
  ROM load, frame run, pixel read, keys, audio drain).
- `mgba-splitscreen/src-tauri/src/emulation.rs` — `EmulationManager`: owns N
  `GbaInstance`s, the `GBASIOLockstepCoordinator` + `GBASIOLockstepDriver`s,
  frame loop, stats, turbo, audio routing. **The link handshake assist lives
  in the C driver (below), armed from here.** It is suppressed by default in
  both frontends — the real fix is the driver's per-transfer hard sync.
- `mgba-splitscreen/src-tauri/src/lib.rs` — Tauri commands + WebSocket server
  (`ws://127.0.0.1:8088`) that streams frames and accepts `load_rom`/`keys`
  commands, so the app is drivable headlessly (`mgba-splitscreen/scripts/*.py`).
- `mgba-splitscreen/src-tauri/src/bin/web_server.rs` — standalone web build (`mgba-splitscreen-web`).
- `mgba-splitscreen/src/main.js` + `index.html` — frontend (video-call grid, input).
- `src/gba/sio/lockstep.c` + `include/mgba/internal/gba/sio/lockstep.h` —
  **upstream mGBA lockstep SIO driver. The FS assist experiment patches here.**
- `mgba-splitscreen/tools/threaded_link.c` — headless C harness: N cores on N real
  threads, canonical mGBA threaded lockstep + selectable in-process
  `GBASIORendezvousDriver` (rendezvous.c). **This is the primary FS
  reproduction/instrumentation tool.**
- `mgba-splitscreen/tools/rendezvous.c`/`.h` — bespoke deterministic in-process SIO
  driver (fork of lockstep.c; no per-transfer hard sync, cycle-locked
  transfers). Mirrors the lockstep.c FS assist.
- `mgba-splitscreen/linktest/` — MULTI-mode link instrument ROM (shows roles, slots,
  RTT, STALL, FRM parity). **Always rule out wrapper/lockstep problems with
  this ROM first.**
- `mgba-splitscreen/scripts/` — Python drivers: `nav_fs.py` (drives FS to the link
  screen), `ws_play.py`, `raw_ws.py`, `linktest_frm.py`, etc.
- `Test Roms/` — owner's legal ROMs (gitignored). Four Swords is
  `Legend of Zelda, The - A Link To The Past Four Swords (U) [!].gba`.
- `PROJECT_LOG.md` — full chronological project log (the long-form history).
- `agents.md` (this file), `to-do.md`, `history.md` — quick-start, remaining
  work, and tried-vs-next log. **Append to `history.md` as you work.**

## Build & run

```bash
cd mgba-splitscreen/src-tauri
cargo build --release                      # ALWAYS release (~10x faster)
./target/release/mgba-splitscreen                   # desktop window (needs a display)
cargo test --release                       # 128 unit + 2 smoke tests
cargo run --release --bin mgba-splitscreen-web -- --players 4   # http://127.0.0.1:8080
# C harness (for FS link experiments):
mgba-splitscreen/tools/build.sh                     # rebuilds threaded_link against libmgba.a
mgba-splitscreen/tools/threaded_link --fs3 "Test Roms/Legend of Zelda, The - A Link To The Past Four Swords (U) [!].gba"
```

First libmgba build takes minutes + several GB RAM. cmake builds happen via the
cargo build script; `mgba-splitscreen/tools/build.sh` re-derives the exact cmake `-D`
defines so the harness struct layouts match `libmgba.a`.

### Android (TV / tablet) — verified working 2026-09-11

The Android app runs the **WASM engine in the system WebView**; no native core is built for
that target. Set these up (user-local, no root) and export them before any `tauri android`
command — the emulator/SDK live outside the repo:

```bash
export JAVA_HOME="$HOME/android-dev/jdk-17"                       # Temurin 17
export ANDROID_HOME="$HOME/android-dev/sdk"                        # platform 36, build-tools 36
export NDK_HOME="$HOME/android-dev/sdk/ndk/27.3.13750724"
export PATH="$JAVA_HOME/bin:$ANDROID_HOME/platform-tools:$PATH"

cd mgba-splitscreen
npx tauri android init                            # once; then commit src-tauri/gen/android
npx tauri android build --debug --target x86_64   # ~5 min cold; needs web/build.sh output
```

Emulator + on-device verification recipe (the emulator needs KVM; `-no-window` is fine):

```bash
export ANDROID_AVD_HOME="$HOME/.config/.android/avd"              # avdmanager writes here
"$ANDROID_HOME/emulator/emulator" -avd tvtests -no-window -no-audio -no-boot-anim \
    -gpu swiftshader_indirect -memory 2048 -port 5554 &
"$ANDROID_HOME/platform-tools/adb" install -r -t <apk>
# Launch through the TV (leanback) launcher, not just the activity:
adb shell am start -a android.intent.action.MAIN \
    -c android.intent.category.LEANBACK_LAUNCHER -n com.coreyb.mgbasplitscreen/.MainActivity
# The app's JS console appears in logcat, tagged Tauri/Console:
adb logcat -d | grep "Tauri/Console"
# To drive the page, forward the debug WebView and use CDP (debug builds only):
adb forward tcp:9222 localabstract:webview_devtools_remote_$(adb shell pidof com.coreyb.mgbasplitscreen)
curl -s http://127.0.0.1:9222/json      # -> ws://127.0.0.1:9222/devtools/page/<id>
```

Ways to get a ROM into the Android app, in order of usefulness on a TV: the `?rom=<url>`
deep link (works, and needs the host to send CORS headers — `10.0.2.2` reaches the host
from an emulator), the Games Library / **File → Load ROM…** picker, and `adb push` plus a
file manager. Verified end to end on the android-36 `android-tv` image: installs, appears
in the TV launcher with its banner, boots the engine, and runs Four Swords in two linked
cores (`?rom=` served from the host with CORS).

## Key gotchas

- **ROM load order**: `core->init()` BEFORE `mCorePreloadFile()` (else
  `core->board` is NULL → segfault).
- **Link attach before boot**: attach lockstep SIO before `reset` or games see
  the cable mid-boot.
- **`GBASIOLockstep*` API lives in `include/mgba/internal/gba/sio/lockstep.h`**
  (upstream moved it out of the public headers); `mgba_bindings.h` must include
  it for the Rust bindings.
- **Logging**: mGBA installs no default logger → `mLog` floods stdout at every
  level. The app installs a WARN-only logger. The harness defaults to WARN too
  (DEBUG SIO = gigabytes per run).
- **Don't trust the ROM for wrapper bugs**: a game crawling on a link-heavy
  screen or a FRM divergence can be the ROM's own rendering (the linktest
  crawl was a per-frame full-screen clear). Always cross-check with the
  linktest ROM.
- **The FS link bug is NOT a mgba-splitscreen wrapper bug.** It reproduces under
  threaded mGBA (canonical model) and under cycle-locked rendezvous. It is
  upstream issue mgba#3286 territory, but our assist work has gotten FS
  further than stock mGBA.

## Four Swords link state (TL;DR — full story in history.md / PROJECT_LOG.md)

- **STATUS (2026-09-08): 2P multiplayer REACHED on the app's real lockstep
  driver.** `threaded_link --fs6` (lockstep + the driver-side inline kick
  gated to round boundaries) reaches real co-op gameplay — two Links in the
  lava-cave room — reproducibly (fs8, fs8b; fs8c pending). The games reach
  mode 2:9 with the kick logging ZERO actual injections and the echo assist
  never armed: the lockstep driver's own pacing (with the AckPlayer sleep
  removal) completes the post-link handshake unaided.
- **The ungated inline FS deadlock kick was itself the fs9/fs10/fs12
  deadlock cause**: it fired mid-round (recv 4-9) and desynced the games' own
  12-transfer round accounting. Gating it to round boundaries (recv==0,
  recv>=12, recv==-1) renders it inert, and gameplay is reached anyway.
- The fix = TWO parts, both in uncommitted `src/gba/sio/lockstep.c`:
  (1) in `GBASIOLockstepCoordinatorAckPlayer` the secondary is NO LONGER put to
  sleep when it acks a transfer — both games observe completions at the same
  cycle (necessary but not sufficient: pure-driver fs9 never broke the 9:2
  stall); (2) the FS deadlock kick gated to round boundaries (recv==0,
  recv>=12, recv==-1) — fires ONLY at a genuine round-boundary deadlock and
  breaks it (fs8c: 2 kicks → char select → gameplay). Committed
  timing-loosen history (`fb2bffbc0` et al.) is also in this branch.
- Remaining: decide the final patch shape (strip the now-uneeded assist stack
  vs keep it gated as a host-armed 4P safety net), revert TEMP traces in
  io.c/sio.c, verify 4P. See history.md 2026-09-08 (late) entry + to-do.md.
