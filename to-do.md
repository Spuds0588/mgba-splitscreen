# to-do.md — What remains

> Ordered by priority within each section. **Aggressively append results to
> `history.md` as you work** so the next session can pick up mid-thought.

## 🔴 2026-09-30 — RetroArch core: MKSC 2P proof + reset-crash fix (this session)

- [x] **Audio-rate fix PROVEN inside real RetroArch with MKSC 2P**: 2×
      SET_SYSTEM_AV_INFO per boot, continuous emulation (2.27M SWI/DMA lines in
      one session), animated title screens pixel-verified in both quadrants,
      P1 driven to the link-flow area (docs/screens/retroarch-mksc-2p*.png;
      evidence story + working launch recipe in history.md 2026-09-30).
- [x] **RESET segfault found & fixed** (reproduced 2/2 before the fix): sp_reset
      now detaches drivers BEFORE coordinator teardown (stale lockstepIds →
      NULL deref in GBASIOLockstepDriverReset) and re-applies the host
      FS-assist policy after CoordinatorInit; lockstep.c re-registers from
      scratch on a stale id instead of crashing. Verified live: RESET survives,
      emulation continues.
- [x] Stall watchdog: sp_run_frame logs STALL state after ~30 zero-step frames
      so future link deadlocks self-report.
- [ ] **Link-wedge fix (top remaining core defect)**: coordinator can deadlock
      ("Primary waiting for players to ack", 0 steps/frame, frozen picture) when
      one player idles minutes in a link-discovery screen while the other sits
      in a non-link menu. Repro + state printed by the new watchdog. Needs a
      re-arm/abort path (coordinator round timeout or sp-level desync→resync).
- [ ] Finish the MKSC 2P link flow to a race screenshot: drive both players
      through MULTI-PAK LINK → TRANSFER (OCR the menus — tesseract-ocr needs
      an elevation-approved apt install; blind pixel-reading hit its limit).
- [ ] Upstream submission (spec step 5): libretro-super PR + docs PR + GitLab
      mirror ask — out-of-band, needs a maintainer; .gitlab-ci.yml is inert
      until the mirror exists.

## 🟢 2026-09-29 — README screenshots + Hide Menu/Full Screen (this session)

- [x] **Hide Menu / Full Screen (F11, `ui_hide`)** shipped + verified live in the
      browser (toggle, Esc-restore, hint pill, persistence, sw v7). Desktop Tauri
      build compiles the same frontend — worth one manual smoke in the app.
- [x] **docs/screens/ + README screenshots section** — real 4P gameplay from
      Mario Kart: Super Circuit (VS race, 4 views), Kirby & The Amazing Mirror
      (co-op hub, group jump, 3 views + fullscreen), and **Four Swords (4-Link
      co-op, room + sword action + Speaker view)**. Screenshot pipeline:
      scripts/shot_server.py + local static host with `?rom=/roms/<name>` symlinks
      (recipe in history.md 2026-09-29; FS session recipe in the (later) entry —
      the `fs_state.dualbystate` title-screen state set skips all menu nav).
- [ ] Kirby: capture the level-interior transition (hub star door → level). The
      door needs the whole group at it + UP while overlapping; P2's cursor ends
      on the pink Kirby that stands closest.
- [ ] Optional: a third game with real linked play for the README (Shining Soul
      II 2P co-op is the best candidate — needs its save-file flow driven).
      **Done for Four Swords 2026-09-29** (see history.md); Shining Soul II still
      open if a fourth game is ever wanted.
- [ ] Clean up the debug/mks* captures in /tmp (they're outside the repo, fine
      to leave) and consider a scripts/README note for shot_server.py.

## 🟢 2026-09-29 — Future feature: single-card (Multi-Pak) sessions — committed, not yet built

The README said "single-card Multi-Pak play" in the Mario Kart caption; an audit
found that's **not what the app does** — every unit runs its own copy of the ROM
and multiplayer goes through each game's link-menu mode (verified live in the
captioned sessions). Corrected the README captions and added a Features note
pointing here. What real single-card would add: the host transmits a multiboot
image (~EWRAM-sized) to P2–P4 over the cable and boots them as guests, so games
whose multiplayer is *only* Multi-Pak (or guests with no copy) work.

**Committed roadmap feature** (owner, 2026-09-29). The project's north star —
compatibility, playability, and stability at the lowest minimum hardware
requirements; accuracy is not the goal (see agents.md) — shapes this design
on purpose:

- **Compatibility:** some games are single-card-only (WarioWare's host-gadget
  multiplayer, per history.md) and are simply unplayable today; guests also
  shouldn't need to own the cart.
- **Playability:** the flow should feel like the real thing — the host starts
  a single-card session, P2–P4 power on with no cart and land in the game via
  the host's TRANSFER.
- **Low hardware floor:** the guest receiver is in-process HLE, so no real
  16 KiB BIOS is required (a BIOS file would raise the floor for users who
  don't have one), and a guest booting nothing holds no ROM copy — per-guest
  memory drops by the full ROM size, which matters on 1–2 GB Android TV
  devices.

Why it doesn't work today:

- **Userspace multiboot exists, our virtual cable doesn't.** The boot ROM's
  `MultiBoot` protocol (entry `0x002C` in `src/gba/hle-bios.s`) runs over SIO
  Normal 8/32-bit mode, and mGBA's core does implement userspace multiboot
  receiving (`GBASIONormalInit` → header handshake → `EWRAM 0x0203FFF0` vector
  → `gba->memory.io[REG_KEYCNT >> 1] |= 0x8000` in `src/gba/sio/normal.c`).
  But the lockstep coordinator only carries **Multi**-mode transfers
  (`GBASIOLockstepCoordinatorUpdateMulti` is the only wire path); a guest's
  `MultiBoot` probe switches its SIO to Normal mode and the lockstep driver
  never forwards those bits, so the handshake never starts.
- **Even the userspace path is only half the story.** The majority of real
  games boot guests through the **BIOS multiboot protocol** (SWI `0x25`
  `MultiBoot` with the ROM header's `0x9C` `0x62 0x02 0x03 0x01 0x9A` — "Multi-Pak
  signature") — e.g. Mario Kart: Super Circuit transfers its link program when
  the host picks TRANSFER. mGBA's `mCoreLoadBIOS` **rejects the real 16 KiB GBA
  BIOS multiboot data** (it requires the `GBA_BIOS_CHECK` checksum over the full
  image, and a complete BIOS is required for multiboot), so an HLE/host path is
  needed.
- **No hook exists in the wrapper.** `GbaInstance::load_rom`
  (mgba-splitscreen/src-tauri/src/emulation.rs) and the libretro splitscreen
  core's `sp_load` (src/platform/libretro-splitscreen/instances.c) both load the
  same ROM into every instance; there is no "guest boots nothing" mode at all.

### Design sketch (for the session that picks this up)

1. **Wire Normal mode through the coordinator** (the prerequisite, also the
   hardest part): extend `GBASIOLockstepCoordinator` (src/gba/sio/lockstep.c)
   with a Normal 8/32-bit transfer path alongside `UpdateMulti`, so bit-exact
   Normal-mode traffic can flow between N instances. The GB/GBC v0.4 link work
   needs the same plumbing.
2. **Host side**: nothing changes — the host boots its full ROM and its game
   drives the transfer (SWI 0x25 on real hardware).
3. **Guest side**: a new load mode where P2–P4 boot with an empty cartridge
   slot and HLE the *receiving* half of the protocol: header/Multi-Pak
   signature handshake over Normal mode, payload streamed into EWRAM, boot at
   the `0x0203FFF0` entrypoint with handoff bits matching `GBASIONormalInit`.
4. **Completion handshake**: the host's game expects the MASTER/SLAVE id
   exchange before showing guest menus; replicate what `src/gba/sio/normal.c`
   does for a single receiver, fanned out to N guests.
5. **Serialization**: the guests' EWRAM image + reception state must join the
   DUALSTATE set (libretro: `sp_serialize`'s blob), or a state saved
   mid-transfer desyncs the games on reload.
6. **Verification**: grow the `linktest` ROM with a MULTIBOOT host/guest pair
   (it already reports link state on screen), then Mario Kart Super Circuit 4P
   via host TRANSFER with guests booting from the cable, then Kirby co-op.

## 🔴 Top priority: Four Swords multiplayer link fix

### 2026-09-11 (final) — FIX CONFIRMED, and it is not FS-specific

The `_hardSync` restore is confirmed working end to end, and the user reports it
fixes **multiple link games, not just Four Swords**. Committed and pushed as
`3a7060bb8`. Everything below this entry is the search history that led here;
keep it for the diagnosis trail but treat the fix as landed.

**Desktop (Tauri) app: links too, with one known wart.**

- [ ] **Remove the "press B, then START again" dance.** In the desktop app the
      host (P1) has to abort the session (B) and re-start linking before the
      handshake completes. The first START alone is not enough. Prime suspect is
      the discovery/retry machine's round counter: the first attempt appears to
      leave it in the post-retry state that a fresh START clears. Fixing this is
      optional polish — filed rather than chased, because the failure mode is
      uncomfortable to reason about and the workaround is trivial for the user.
- [ ] **Intermittent LOZ FS link failure.** Even with the fix, Four Swords
      occasionally still fails to link although the same steps usually work.
      Investigate after the wart above; a repro needs the games parked at the
      link screen with SIO payload logging, **not** screen classification
      (the native harness's screen heuristics are known-bad — see below).
- [x] Desktop app defaults the FS assist/kick OFF (`SetFSSuppressed`), matching
      the browser. Verified live: `[mGBA] FS assist: suppressed by host`.

**Known desktop bug found while testing the fix (unrelated to SIO):**

- [ ] `quit_game` → `load_rom` over the app's WebSocket **wedges the emulator**:
      after the pair, every instance reports 0.0 fps and the app emits no frames
      at all (the frame loop stops logging too). Workaround is to restart the
      process. Reproduce with `mgba-splitscreen/scripts/raw_ws.py` (`{"type":"quit_game"}`,
      then `load_rom`) before blaming the link for a frozen session.

**Web deep links — DONE** (`?players=N`, `?rom=<url>`):

- [x] The web build accepts `?players=1-4` (applied before the engine boots) and
      `?rom=<url>` (fetched and started immediately). Verified live on a fresh
      origin: `?players=4` -> 4 linked cores; `?players=4&rom=fs_rom.gba` -> all
      four booted to Four Swords `CHOOSE A FILE` in sync; `?players=9` warns and
      falls back to 2; `?rom=missing.gba` reports `HTTP 404 File not found` and
      leaves the launcher usable. Documented in the README.
- [ ] **Known limit:** `?rom=` is a plain `fetch`, so a cross-origin URL needs
      permissive CORS on that host (and the browser refuses a bare file). Options
      if we want arbitrary links to work: ship a tiny CORS-enabled hosting recipe,
      or let the Pages site pull ROMs from a directory the user drops beside
      `index.html`. Also unproven: extremely large ROMs (32 MiB) over a slow link
      load with no progress indicator — the status line says `Fetching: <name>…`
      but has no percentage.

**Branding: DONE.** The one-name rule landed as "mgba-splitscreen" (the GitHub repo
name, kept so Pages URLs and existing clones survive). Directory, crate, npm package,
bins, `productName`, identifier, WASM artifacts and the `_dbs_` bridge prefix all
renamed; v0.3.0 published for Linux/macOS/Windows. The only intentional survivors are
historical mentions in `history.md`/`PROJECT_LOG.md` and the `.mgbastate` extension's
back-compat acceptance of the old `.dualbystate`.

### 2026-09-11 (later) — the H1 experiment was the regression; reverted

`git diff e191ddf9b..HEAD -- src/gba/sio/lockstep.c` is only three things:
`UNLOCKED_INTERVAL 4096->8192`, the `nextHardSync` restructure (both from
upstream's "Loosen timing where possible", the patch that *made FS progress* on
2026-08-17), and `_hardSync(...)` commented out at the end of `FinishMultiplayer`
— the "H1 experiment" from `a9ca9b441`. H1's own outcome line is "the handshake
still cycles", so it did not fix anything; it *removed* the end-of-round
realignment that had just gotten FS 2P past the linking screen. **Restored the
`_hardSync()` call.** Smoke checks: 2P and 4P `--fs10` both stall-free (0
"did not receive data"), 4P wire IDs correct (master `200B` id0, children
`609F/60AF/60BF` ids 1/2/3).

**Do not trust `mgba-splitscreen/tools/threaded_link` screen verdicts.** Its A+B "confirm"
presses B on the link screen, which *cancels* the link, and `dump_ppm(g_cur)`
reads a lagging snapshot: the `fs_ab_before` dump is the intro cutscene, and what
it then watches is the **title attract demo**, not gameplay. Judge progress on the
**live** browser screen (`preview_screenshot`), which is also the environment the
user reports on.

**DONE — verified 2P in the browser.** With the restore in the WASM: import a
pre-link state set, START on both, tap A → **CHOOSE A STAGE** → tap A again →
**both Links in the dungeon, in sync**, zero `MULTI did not receive data`. Real
linked play. Do not press B anywhere: on the link screen B cancels the link.

**Next: 4P.** Four games must sit at the link screen together before the A taps.
The imported 4P state set is itself inconsistent (one core restores on the linking
screen, the rest at the title) and solo-keyboard input desyncs four games through
the menus, so drive them with Solo Keyboard **off** (P1 W/S/A/D+K/J+Space, P2
arrows+M/N+Enter, P3 T/G/F/R+Y/U+Z/X+1/2, P4 I/Q/C/E+P/brackets/comma/period+3/4)
or fix the state set first. If 4P still cycles once all four are at the link
screen, the remaining bug is FS's 4-unit expectation, not SIO (`tests/linktest_4p.rs`
proves the link layer at 4P).

### 2026-09-11 — read this first: the link layer is exonerated

**The lockstep SIO layer is verified correct at 4 players** in the exact
cooperative model the browser uses (new `tests/linktest_4p.rs` runs the linktest
instrument ROM through `EmulationManager`: `TX 1251 / RX 1250`, `STALL 0`,
`LINK ACTIVE - 4 PS`, all four units agree on `SIOMULTI0-3`, 60 fps each). Do not
spend more time on `SIOMULTI` mapping or transfer pacing — both are proven.

**The FS assist + deadlock kick does not help.** A/B at 4P on the same flow:
assist ON → 0 stalled transfers, 4 injections, still on the linking screen;
assist OFF → 0 stalled transfers, still the same screen. Both are alive (the
link screen's blink animates). The assist is unproven, large, and uncommitted.

**Recommended posture while debugging:** run with `MGBA_SPLITSCREEN_FS_ASSIST=0`. The
assist has never advanced the game; with the Round-6 ack barrier in place the
unassisted path is already stall-free, so the assist only muddies the trace.
Decide explicitly whether to rip it out before it grows further.

**Immediate next step:** a *fresh-boot* baseline. Every A/B so far used the
`mgba-splitscreen.dualbystate` import, which restores into a mid-handshake freeze and so
cannot reach gameplay — that confound is why results keep contradicting each
other. Drive FS from boot (file-select → name entry → CHOOSE A GAME → Four
Swords) at 2P and 4P: if 2P reaches name entry and 4P does not, the bug is in
FS's 4-unit expectations. `mgba-splitscreen/scripts/link_test.py` already drives that
menu path over the websocket.

Goal: get *The Legend of Zelda: A Link to the Past / Four Swords* past the
linking handshake into actual 2P gameplay (and ideally 4P). Status of the
individual screens:

| Screen | Status |
|---|---|
| Boot → file select → name entry → save → game select | ✅ works (nav_fs.py) |
| FS title → START → "Linking…" screen | ✅ works |
| Post-link name entry / second handshake | ✅ works on lockstep (fs8/fs8b) |
| **Character select → actual gameplay** | ✅ **REACHED (2P)** — fs8, fs8b, mode 2:9 stable; real co-op screenshots verified |

**2026-09-08 (late) — 2P gameplay reached on the real lockstep driver.**
`threaded_link --fs6` runs reach mode 2:9 with **zero** actual kick injections
(the round-boundary gate blocks all 112 attempts) and the echo assist never
armed (armed=0). Verdict: the ungated inline kick (pre-2026-09-08) was the
fs9/fs10/fs12 deadlock cause — it fired mid-round at recv=4-9 and desynced
the games' round accounting. The load-bearing driver change is the AckPlayer
**sleep removal** (secondary no longer sleeps on ack; completions land on the
same cycle for both games). Full detail: history.md 2026-09-08 entries.

### 2026-09-09 update — real-app verification IN PROGRESS (major progress)
- **TWO root causes found + fixed (see history.md 2026-09-09):**
  1. `GBASerialize` never zeroed the state → garbage `hw.unlCartFlags` (0x6E29 →
     type 9) → "Save state expects different bootleg type" on EVERY state import
     → bootleg section skipped → P1 executed garbage (PC at 0x0D000000) and the
     kick's all-players gates could never pass. Fix: `memset(state, 0, ...)` at
     the top of GBASerialize. Also patched the existing mgba-splitscreen.dualbystate
     (zeroed 2 bytes at 0x2C2 per blob; backup /tmp/mgba-splitscreen_orig.dualbystate).
  2. State-load freeze (fr=0): driver reset kept stale restored event queues →
     first HARD_SYNC re-slept the secondaries forever. Fix: drop queue + rebuild
     freelist + clear asleep in GBASIOLockstepDriverReset. (This edit was
     compile-broken for several runs — the "queue-clear didn't fix it" verdict
     was from a binary that never contained it.)
- **Server-side verification of the fixed stack: ROM+state load 200, no bootleg
  warnings, all 4 cores 60fps (old lib: sleep:[TTTT] freeze), identify OK,
  `FS kick: invoking` x32, `kicked deadlock (phase=2 recv=13 got12=1)` x3 —
  the kick now injects and games consume got12.**
- Rebuilt: WASM (10:54, into mgba-splitscreen/web + src), native lib + server (10:43,
  running as pid 91446), harness (10:44). build.sh dies silently when a
  concurrent build clobbers build-wasm/ — run make + emcc manually then.
- App stability: mgba-splitscreen-web runs indefinitely via `setsid nohup ... &` (earlier
  "crashes" were shell process-group teardown, not the app). No segfault seen.
- raw_ws.py audio-tag bug fixed (reader kept audio chunks as `latest` → spurious
  None frames) + auto-reconnect + cap_frames.py helper.
- nav_fs.py: detect() name-vs-saving reorder + harness-style "continue anyway"
  flow. Nav reached FS select on the server but post-save waits misclassified;
  a bottom-button-band discriminator (name=11 clusters vs file=2) is designed
  but NOT wired in — bypassed by the user's 4P pre-link save state instead.
- Save-state set export/import added: web_server /state (GET/POST), Tauri
  export/import_state_set, UI File → Export/Import State Set (DUALSTATE blob,
  wasm + socket + Tauri modes). Tested round-trip.
- WASM web build REBUILT (Sep 9 08:56) with the lockstep fix — the browser
  engine now carries it; GitHub Pages ships these tracked files on commit.

### Immediate next steps (this session)
1. [x] fs8c 3rd confirmation — gameplay reached after a real 60s 9:2 stall + 2 boundary kicks.
2. [~] VERIFY IN THE REAL APP via the user's 4P pre-link save state — server side
     FULLY VERIFIED (round 4, app_run11.log: clean import, 60fps x4, kick
     injecting x2+). Browser retest pending the mgs_reset_sio + decline-diagnostics
     fix (round 4, below).

### 2026-09-09 (round 4) — browser P1 divergence: WASM import missing driver reset
- Root cause: `emulation.rs load_state_set` resets every driver after load;
  `mgba_splitscreen_web.c mgs_load_state_bytes` (browser) did NOT — stale restored
  queues/asleep flags diverged P1's handshake (polled 0x0D000000, kick gates
  blocked, zero injections; server same file: 0 OOB + 219 injections).
- Fixes (rebuilt, server pid 119307): `mgs_reset_sio()` export + wired into both
  wasm import paths; kick decline diagnostics (`FS kick declined (gate):
  P1(mode lstat ssub role ph recv got12)...` rate-limited); session-log flush
  timer bug (re-arm unconditionally + sendBeacon on unload — logs were dying in
  the buffer); wasm MIME (`application/wasm`). Full detail: history.md round 4.
- NEXT browser test: hard reload → 4P → ROM → Import State Set → START all 4.
  Watch for `kicked deadlock` (win) or `FS kick declined (...)` (tells us which
  gate + per-player values; P1's mode/lstat will reveal corruption instantly).
2.5. [ ] Decide final patch shape and ask the owner:
     (a) MINIMAL: strip the FS-assist stack (kick/echo/unstick + gates) from
         lockstep.c, keeping only the AckPlayer sleep removal — cleanest, but
         DISPROVEN by fs9 (pure driver never breaks the 9:2 stall); or
     (b) KEEP the round-boundary-gated kick + mode-9/lstat gates — PROVEN
         (fs8/fs8b/fs8c all reach gameplay; fs9 without it fails).
         => evidence says (b). Echo/unstick stay host-armed (off unless armed).
3. [x] Reverted TEMP traces in src/gba/io.c + src/gba/sio.c; rebuilt lib+harness.
4. [ ] fs10 = fs6 on the CLEANED tree — confirm gameplay still reached.
5. [ ] Verify in the real app (mgba-splitscreen-web / nav_fs.py + manual key events) if
     feasible; then 4P.
6. [ ] Commit sequence on `fs-link-loosen-timing`: (committed timing-loosen
     history stays) + the lockstep.c fix as one clean commit; decide the fate
     of the --fs* harness modes (keep threaded_link.c + rendezvous.c as dev
     tools vs delete).

### 2026-09-06 status (session findings — read history.md first)
- **Mode question settled:** the games stay in MULTI the whole run (SIOCNT
  bits 12-13 = 10; the old "post-name NORMAL32 handshake" premise was a
  misread of `siocnt>>14` — bit 14 is IRQ enable). NORMAL read-back fix is
  harmless but not the path.
- **32-bit-write hypothesis refuted** (zero `SIO32W` traces; the game uses
  16-bit SIOMLT_SEND + SIOCNT writes).
- **Two REAL assist bugs fixed:** `_fsAssistNormalize` was rewriting
  self-consistent game table entries (every pair sums to exactly 0xFFF2;
  block checksum target is 0xFFF1) → now log-only; the echo was overwriting
  the slave's real table data → now FEFE-probe-only. Both changes are in
  `rendezvous.c` (and the lockstep.c mirror still needs the same treatment).
- **Remaining blocker:** post-name phase — games' recvIdx freezes (master 0,
  slave = initial 13), the EWRAM recv handler stops being invoked, the master
  polls [0x4000128] forever without writing SIOCNT. SIO IRQs still fire with
  IE enabled. Driver transfer path verified healthy.

Next experiments (see history.md for reasoning):
1. Disassemble the CALLERS of the EWRAM handler (0x02030590) and ROM handler
   (0x800C54C) to find the post-name gate that stops the recv path.
2. Instrument the EWRAM handler entry (any invocation → WARN) to confirm it
   stops being called.
3. Sync the fixes into `src/gba/sio/lockstep.c` (the mirror driver) once
   the rendezvous version is confirmed.
4. Revert TEMP traces (io.c SIORD/SENDW/SIO32W/IEW, sio.c SIOIRQ — all
   marked TEMP) before any merge.

Uncommitted work in the tree (branch `fs-link-loosen-timing`):
- `src/gba/sio/lockstep.c` + `include/.../lockstep.h` — FS assist (lazy cart
  ID, host-armed echo, stalled-round echo, transferActive unstick),
  NORMAL8/32 master read-back fix.
- `mgba-splitscreen/tools/rendezvous.{c,h}` — mirror assist + `_fsAssistNormalize`
  (0xFFF1/0xFFF3 checksum-drift correction) + `ReadMultiRegs` + `SetFSArmed`.
- `mgba-splitscreen/tools/threaded_link.c` — `--fs3` (post-link flow, assist on),
  `--fs4` (same, no assist), `--fs5` (no assist + full SIO DEBUG), button
  probes, richer `dump_fs_state`.
- `src/gba/io.c` — TEMP instrumentation (SIOCNT/SIOMULTI read trace, RAW
  SIOCNTW log). **Must be reverted/cleaned before any merge.**

Next experiments (see `history.md` "What to try next" for full reasoning):
1. Re-run `--fs3` vs `--fs4` after the NORMAL8 read-back + normalize changes
   to see where the post-name handshake stands (the last session's fsAssist
   results were never logged).
2. If the post-name screen still stalls: trace the slave's SIOMULTI **reads**
   and its branch on the received value during the second (0xFFF3) handshake
   (P1 in PROJECT_LOG).
3. When the game reaches gameplay: verify 4P, then decide whether to keep the
   assist permanently (it is FS-cart-gated + host-armed, so it can't touch
   other games) or narrow it further.
4. Clean up: revert TEMP io.c instrumentation, decide on keeping the assist in
   lockstep.c, document + commit the working state, write a PR-quality patch
   for upstream mGBA if it's genuinely driver-side.

## 🤖 Android (TV, tablets, Chromebooks)

### 2026-09-11 — Android TV shell VERIFIED END TO END

The Android app is a Tauri v2 shell around the **same WASM engine the web build ships**,
running in the system WebView. No native core is built for Android (`build.rs` returns
early, src/lib.rs cfg-gates the modules/commands), so a build is minutes rather than an
NDK cross-compile of the whole emulator. Verified on the `android-36;android-tv;x86_64`
system image: installs, launches from the leanback launcher, boots the engine, and runs
Four Swords in two linked cores — screens fully rendered, driven by the `?rom=` deep link
fetched from the host over CORS. Build/emulator recipe is in `agents.md`.

- [x] Leanback entry (`LEANBACK_LAUNCHER`), 320x180 `tv_banner.png`, `isGame`.
- [x] `uses-feature android.hardware.touchscreen` declared **not required** — without
      that, Android/Play treat an undeclared touchscreen as required and filter the app
      off every TV before it can be installed.
- [x] `tauri.android.conf.json` swaps `strip-web-engine` for `stage-web-engine`, so the
      Android bundle ships the engine the desktop bundle deliberately removes.
- [ ] **Release APK signing is undecided.** Only a debug APK has been built. A
      distribution build needs a key in `gen/android/keystore.properties` (gitignored);
      generating a throwaway key would make future updates impossible to install over it,
      so this needs your real key (or CI secrets).
- [ ] **Release (minified) Android build is untested.** The generated project sets
      `isMinifyEnabled = true` + proguard for release; only the debug variant has been
      exercised. Build the release variant before trusting it.
- [ ] **Touch controls do not exist.** `pointerdown` is only wired to audio unlock, so a
      tablet or touchscreen Chromebook needs an on-screen pad. The input plumbing is
      ready for it: `P1_MAP`..`P4_MAP` already turn buttons into the bitmask that
      `mgs_set_keys` consumes.
- [ ] **A TV remote currently drives Player 2**, because P2's defaults are the arrow keys
      plus Enter and that is exactly what a D-pad remote sends. Add an explicit TV mode so
      a remote (and a single gamepad) can drive Player 1.
- [ ] **On-device performance is unmeasured.** The emulator host (x86_64 + KVM +
      swiftshader) says nothing about an Amlogic/Google-TV-class SoC. Expect 1-2 linked
      cores to be the honest target on a TV stick, 4 on Chromebooks/tablets — measure on
      real hardware before promising 4-up on TV.
- [ ] **The 256 MB fixed WASM heap is the top risk on low-RAM devices.**
      `web/build.sh` links `-sINITIAL_MEMORY=268435456 -sALLOW_MEMORY_GROWTH=0`; that held
      on the emulator, but a 2 GB TV stick may reject it. Consider a lower initial size
      and/or `ALLOW_MEMORY_GROWTH=1` for the Android/mobile build.
- [ ] `tauri android init` regenerates `gen/android`; the manifest edits (and banner) are
      committed, but re-running init can overwrite them — diff after any re-init.

## 🟣 v0.4-v0.6 expansion roadmap

The next versions are intentionally staged: stabilize local multi-system support before adding online transport.

### v0.4 — local multi-system foundation

- [ ] Make ROM loading platform-aware for GBA, GB, GBC, and supported GBX files.
- [ ] Enable the native GB core and introduce a platform-neutral emulator-instance model.
- [ ] Support one ROM per local player, including Pokémon Red on P1 and Blue on P2, with separate save identities.
- [ ] Validate link topology before launch: GBA supports 2–4 players; the existing GB lockstep path supports two devices.
- [ ] Refactor video dimensions, aspect handling, audio rates, and audio buffers per instance.
- [ ] Replace browser ScriptProcessor audio with AudioWorklet where supported, retaining a compatibility fallback.
- [ ] Add browser output-device selection with capability detection and Android system-route fallback.
- [ ] Add tests for ROM detection, GB/GBC loading, independent saves, and topology validation.

### v0.5 — host-star online play and external sidebar

- [~] v0.5 foundation: add the optional PeerJS host-star transport; host owns emulation and guests send input while receiving the latest host frame.
- [ ] Harden the transport with bounded queues, sequence numbers, timestamps, reconnect handling, rate limits, host validation, and adaptive video/audio encoding.
- [x] Prototype magic-link invitations with Web Crypto tokens, fragment-contained bearer secrets, 10-minute expiry, and first-join single-use invalidation.
- [ ] Add explicit host approval/revocation, reconnect/resume policy, and a private signaling/auth service before treating online play as production-safe.
- [ ] Add an opt-in URL-controlled iframe sidebar with origin labeling, sandboxing, focus isolation, and HTTP/mixed-content warnings.
- [x] Add the installable PWA shell: manifest, install metadata, service-worker registration, shell cache, and deployer-compatible response headers.
- [x] Add QR-code invite sharing with URL copy, PNG download, and native Web Share fallback.
- [x] Issue independent single-use invite capabilities per guest slot so unfilled seats remain joinable.
- [ ] Handle blocked embeds and camera/microphone/clipboard/fullscreen permissions explicitly.

### v0.6 — handheld and phone packaging

- [ ] Add touch controls and explicit TV/handheld input modes. 2026-09-26:
      first slice DONE — GBA-style touch overlay (`touch-controls.js`),
      auto-on for touch devices and magic-link guests, verified end to end.
      Remaining: remappable touch layout, haptics strength, Android TV D-pad
      focus mode.
- [ ] Normalize Android key/gamepad handling, including safe BACK behavior.
- [ ] Build signed arm64 and armv7 APKs and verify phones, tablets, handhelds, external displays, and TV.
- [ ] Revisit the fixed WASM heap and low-memory WebView behavior.

## 🟠 mgba-splitscreen app / web remaining work

- [ ] Root-cause the one observed tokio-worker segfault (`segfault at 4a8` in
      `mgba-splitscreen-web`): suspected cross-thread `load_rom` (tokio) vs `run_frame`
      (emulation thread). Needs sustained-play re-testing now that logging and
      the release build are fixed.
- [ ] Drive FS to *actual gameplay* at 4P and confirm the link-heavy title
      select stays smooth there.
- [ ] Audio routing — DONE for desktop ALSA; verify web audio parity and the
      per-player source menu end-to-end. 2026-09-26: in-browser silence fixed
      (pump drained after reading the count, so it never delivered a sample);
      online guests now receive host video AND audio over PeerJS. GB/GBC rate
      reporting in mixed mode still assumes 32768 — verify a GB game's audio
      pitch on the web build when convenient.
- [x] 2026-09-28: guest streaming v2 (real QR 2P test feedback). Per-seat JPEG
      video (~8-9KB/frame, 30fps cap, host zero-copy seat provider) replacing
      the 300KB all-players RGBA flood that stranded guests minutes behind at
      3-4 fps; guest renders ONLY their own seat; menubar/overlay/solo-badge
      hidden via body.guest-session; audio coalesced ~30ms (rate headers
      stripped, one fresh header) instead of dropped, guest backlog capped at
      ~500ms oldest-drop. Verified live 2-tab (seat isolation by luma, input
      spy, audio buffered pinned at cap). Watch for tester reports on real
      phones; PeerJS is already WebRTC so no transport change is needed.
- [ ] Online beta follow-ups: show guest fps/latency somewhere subtle (probe
      exists: `mgbaOnline.videoStats()`), JPEG quality/fps adaptivity on slow
      links, and test >2 players sharing one host uplink.
- [ ] **Cross-platform play: 6-digit join codes (PLANNED 2026-09-28 — full
      implementation spec in `mgba-splitscreen/JOIN_CODES.md`: files, message
      formats, UI details, edge-case checklist, verification plan, suggested
      build order. Rationale in history.md.)** Magic links are fine for QR/DM but useless
      verbally; the target session is e.g. Android gaming handheld + phone
      browser + desktop + browser all in one game. All platforms already
      speak the same online protocol (Android APK and desktop ship the same
      frontend; Android/desktop-web run the WASM engine, desktop-native hosts
      via its onFrame path) — the gap is discovery/join UX only. Phases:
      1. Host side: "Host Online Session" shows a big 6-digit code (random,
         retried on PeerJS ID collision) alongside today's QR/link. Host's
         PeerJS ID is DERIVED from the code (`mgs-<code>`), so a guest needs
         only the code — no signaling database. Ephemeral by nature (the ID
         exists only while hosting; collisions retry, squatting is possible
         but pointless at beta scale — revisit with a tiny KV registry if it
         ever bites).
      2. Host-side consent: incoming connections prompt "Someone wants to
         join with code XXXX — approve seat P2/P3/P4?" (guessing a code gets
         you a prompt the host declines, not silent entry; seats assigned on
         approval, replacing the pre-claimed per-slot invites).
      3. Guest side: "Join with Code…" keypad (inputmode=numeric, big
         buttons, paste support) in Online menu on every platform + deep link
         `?online=join&code=NNNNNN` for QR/copy; keep old token URLs working.
      4. Hardening (only if needed): free TURN fallback for strict-NAT pairs
         (PeerJS default is STUN-only — some phone-hotspot pairs fail today),
         code registry service if squatting/collisions become real, adaptive
         bitrate per guest.
      5. Done = a 4-way mixed-platform session (Android handheld + phone
         browser + desktop app + desktop browser) joined by code in under 30s
         each, verified like the 2026-09-28 2-tab sweep.
- [ ] Gamepad support for players 3–4 in the browser (Gamepad API — desktop
      webview works; check browser build).
- [ ] If WebView can't composite 30 FPS on low-end hardware: native
      (non-webview) renderer for the desktop app.
- [ ] Web version on GitHub Pages is live; keep it in sync with the desktop
      feature set (turbo, save states, audio source menu…).
- [x] 2026-09-26: online multiplayer (PeerJS host-star + invites) shipped to
      Pages from `master`, menu labeled beta; crashes auto-file a prefilled
      GitHub issue (`crash-report.js` + `report-issue.html` rescue page) with
      game/system/log details. Watch the `crash-report` label for tester reports.
- [x] v0.4 first slice: widen ROM discovery/pickers/folder scans/URL validation to GB/GBC/GBX and enable the native GB core as groundwork. WASM detection and dynamic video metadata are implemented; the browser WASM path now wires mGBA's two-device GB/GBC lockstep coordinator. Native Tauri GB runtime/link support and independent save identities remain follow-up work.
- [ ] Separate ROMs per linked player (for example Pokémon Red/Blue) is deferred: Oracle of Ages/Seasons do not use a link cable, and Pokémon trading is outside the current audience/use case.

## 🟡 Future dev options (documented, not built)

- **2×2 link groups** — two independent 2-player links (P1–P2, P3–P4) in one
  process, or arbitrary linked/independent core mixes.
- **Per-player screen toggle** — show one player's screen at a time (prereq
  for RetroAchievements-style play before any RetroArch port).
- **Pop-out windows** — each player's screen in its own OS window (multi-screen
  / streamer layouts). The threaded harness infrastructure (`threaded_link.c`)
  is a stepping stone here.

## 🟩 RetroArch (libretro) core — DONE (local), verified in RetroArch 2026-09-29

Full research + implementation-ready spec in `mgba-splitscreen/RETROARCH_CORE.md`:
libretro is single-instance, but the TGB Dual core proves the model (N emulators
inside one core + `retro_load_game_special` subsystem for multi-ROM + layout/audio
core options), and our sequential frame loop in `emulation.rs` ports to C directly.
The fork's `lockstep.c` (FS assist + gated kick) ships to RetroArch users for free.

- [x] Phase 1: skeleton core (`src/platform/libretro-splitscreen/`, copy of upstream
      `src/platform/libretro/libretro.c` renamed) loading one ROM in RetroArch.
- [x] Phase 2: N instances + per-player RetroPad input + composite video + audio
      options, link compiled out.
- [x] Phase 3: wire `GBASIOLockstepCoordinator` + sleep-flag frame loop; linktest
      ROM passes 4P (`LINK ACTIVE - 4 PS` in the master quadrant, verified via
      UDP screenshot inside RetroArch; docs/screens/retroarch-4p-linktest.png).
- [x] Phase 4: subsystem manifest (2/3/4-player), core options (layout, audio,
      FS assist), save-state size discipline, SRAM per player, `.info` file.
- [x] Phase 5 (local): `.gitlab-ci.yml` written (copy of libretro/mgba's, adapted:
      CORENAME mgba_splitscreen, -DBUILD_LIBRETRO_SPLITSCREEN=ON) — only builds
      once the repo is mirrored on libretro's GitLab.
- [ ] Phase 5 (out-of-band): libretro-super `dist/info` PR, libretro-docs PR,
      GitLab mirror request (Discord #programming — the buildbot only builds
      mirrored repos), nightly verification in the Core Downloader. Optional:
      hook the core into `release.yml` so testers get .so/.dll artifacts early.
- [ ] Later: GB/GBC 2P subsystem, determinism testing for the
      `savestate_features = "deterministic"` claim, run-ahead/netplay caveats doc,
      Mario Kart 2P + Four Swords 4P end-to-end in RetroArch (expected to work;
      the core inherits the app-proven lockstep.c).

### 2026-09-10 (round 6) — the link-screen assist was never armed; state import is a dead end
- **Fixed:** `GBASIOLockstepCoordinatorSetFSArmed()` had exactly one caller in
  the tree — the test harness. Neither `emulation.rs` (desktop + web server)
  nor `mgba_splitscreen_web.c`/`main.js` ever armed it, so `_fsAssistTick`'s discovery
  echo returned at its guard and was dead code in the app and the browser.
  Now self-armed in `_fsAssistKick` when the FS cart is identified and every
  attached player is in link-screen mode (IWRAM 0x6D10 == 9). Watch for
  `FS assist: armed at the link screen (all N players in mode 9)`.
  Caveat: 2P gameplay WAS reached in the harness with `armed=0`
  (2026-09-08 entry above), so the self-arm is a needed gap-fix, not proof by
  itself that the cooperative model completes the discovery.
- **The user's `mgba-splitscreen.dualbystate` cannot reach gameplay from any model.**
  It is captured mid-handshake (mode=9, lstat=0, ssub=3; P1 role=8 phase=2
  recv=13); after restore no transfers ever resume (native repro, threaded
  harness `--fs11`, browser) and every game parks on "Linking with other
  systems… Please wait a moment." with `mlt=FFFF`. The core state carries the
  SIO registers but not the coordinator's pending transfer/ack events, and the
  round-4 driver reset deliberately aborts them. **Test from a fresh boot +
  navigation, not from this state.**
- **New tool:** `src-tauri/tests/fs_link_repro.rs` (ignored) mirrors a browser
  session natively against the same cooperative model: 4 players, FS ROM,
  DUALSTATE import, START, A+B confirm, per-player screenshots to
  `/tmp/fs_repro_*.ppm`. `FS_LINK_PLAYERS` / `FS_LINK_STATE` / `FS_LINK_TAG` /
  `FS_LINK_CONFIRM_MS` override the flow. Faster than a WASM rebuild for
  anything that is front-end-independent.
- [x] Teach `_fsAssistKick` the post-retry reset signature (phase=0)
      — done round 7 (phase-sig gate accepts 0/1/2; injection lifts st[1]
      0->1 to re-enter a reset machine).
- [x] Fix the silent allComplete refusal (recv==13 && got12==1 froze the
      kick with no log) — done round 7 (frozen-at-13 is kicked like 11/12;
      "already-complete" decline logged while recv moves).
- [x] Disable the immediate echo engage — round 7 evidence says it broke the
      per-slot checksum validation (master echoed to every slot); raw-data
      discovery restored.
- [x] Set the link-status success latch + char-select gate in the kick
      (`iw[0x0FC3]|=0x40`, `iw[0x0FC8]=2`) — round 7.
- [x] Kick log: print ssub / lstat / fC3 / fC8 per player — round 7.
- [ ] STATIC ANALYSIS (next work item): the ROM's ssub-4 A-confirm — all
      four machines now HOLD at phase=1/ssub=4/fC3=40/fC8=2 but the A press
      never advances them. Trace the link-status machine (0x03000FC0, state-5
      payload check 0x8037b5c) and the >120-frame active-link counter the
      A-check is gated on to find what else must be set (or what keeps the
      counter from counting) before the games leave the linking screen.
- [ ] If 4P still diverges from a fresh boot, diff per-player MULTI delivery
      in `_setData`/`AckPlayer` against the 2P-proven run (4P logs
      `mlt=FFFF,FFFF,FFFF,FFFF`).

## GB/GBC v0.4 scope (2026-09-16)

- [x] Ignore the personal `Test Roms/` collection and generated save/state artifacts.
- [x] Verify GB and GBC ROM detection, two-instance creation, 160x144 rendering, and cooperative stepping in the rebuilt WASM bridge.
- [x] Add a two-endpoint serial probe for diagnostics; retain it as smoke coverage only, not as a substitute for an in-game transfer.
- [x] Defer separate-ROM linked sessions. Oracle of Ages/Seasons exchange passwords rather than using the cable; Pokémon trading is outside the target audience for now.
- [ ] Verify a game-specific GB/GBC cable transaction if a suitable supported test flow becomes available.
- [ ] Wire the native Tauri GB/GBC instance/link model before advertising desktop GB/GBC linking.

## Housekeeping

- [ ] Cosmetic: the `/session_log` POST 405s on GitHub Pages every load (static
      host has no backend); the console-capture self-disables but one warning
      line still logs. Could gate the POST on a successful first response.
- [ ] The QR library is pinned to qrcode@1.4.4 (last release shipping the UMD
      `build/` bundle). If it ever disappears from CDNs, vendor the file into
      `mgba-splitscreen/src/` (55 KB).

- [ ] PROJECT_LOG.md is the long-form log; `history.md` is the tried-vs-next
      log. Keep both in sync at session boundaries.
- [ ] The `--fs*` experiment modes in `threaded_link.c` are dev tools; decide
      which to keep vs delete once the fix is found.
- [ ] Branch hygiene: `fs-link-loosen-timing` carries the uncommitted assist
      work + the `a0647ffac` timing-loosen cherry-pick. Plan a clean commit
      sequence once the fix is confirmed (the cherry-pick may be dropped if
      the assist makes it unnecessary).
### Release pipeline — verify before the next tag

- [x] 2026-09-28: v0.3.1 released and VERIFIED: all 8 assets landed (deb, rpm,
      AppImage, dmg, msi, NSIS exe, universal APK 27.1MB, arm64 APK 8.9MB).
      Android builds joined the pipeline: the gen/ Gradle project only works
      through the Tauri CLI (it generates the gitignored
      tauri.settings.gradle with machine-specific cargo paths, and its rust
      plugin's android-studio-script task needs the CLI's socket — raw
      ./gradlew fails with ConnectionRefused); two `tauri android build --apk`
      passes (plain = universal all-ABI, --target aarch64 = phone variant)
      with the output copied between them. Release signing via env vars:
      ANDROID_KEYSTORE_B64 secret (stable identity, upgrades install over old
      builds) or an ephemeral CI key (fine for testing, needs uninstall
      between releases). workflow_dispatch runs all builds without publishing
      for branch validation. No secrets are set yet — consider adding
      ANDROID_KEYSTORE_B64 before the release leaves prerelease status.
- [ ] Any JavaScript embedded in `tauri.conf.json` runs through a shell on
      Linux/macOS but is spawned directly on Windows. Keep such work in a
      script file under `scripts/` invoked via an npm script.
- [ ] The `Cache Rust build` key includes the package name, so the rename
      forced cold Windows/macOS builds (~10-20 min). Expected, but it makes a
      tag run look slow the first time.
