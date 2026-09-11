# to-do.md — What remains

> Ordered by priority within each section. **Aggressively append results to
> `history.md` as you work** so the next session can pick up mid-thought.

## 🔴 Top priority: Four Swords multiplayer link fix

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

**Do not trust `DualBoy/tools/threaded_link` screen verdicts.** Its A+B "confirm"
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

**Recommended posture while debugging:** run with `DUALBOY_FS_ASSIST=0`. The
assist has never advanced the game; with the Round-6 ack barrier in place the
unassisted path is already stall-free, so the assist only muddies the trace.
Decide explicitly whether to rip it out before it grows further.

**Immediate next step:** a *fresh-boot* baseline. Every A/B so far used the
`dualboy.dualbystate` import, which restores into a mid-handshake freeze and so
cannot reach gameplay — that confound is why results keep contradicting each
other. Drive FS from boot (file-select → name entry → CHOOSE A GAME → Four
Swords) at 2P and 4P: if 2P reaches name entry and 4P does not, the bug is in
FS's 4-unit expectations. `DualBoy/scripts/link_test.py` already drives that
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
     the top of GBASerialize. Also patched the existing dualboy.dualbystate
     (zeroed 2 bytes at 0x2C2 per blob; backup /tmp/dualboy_orig.dualbystate).
  2. State-load freeze (fr=0): driver reset kept stale restored event queues →
     first HARD_SYNC re-slept the secondaries forever. Fix: drop queue + rebuild
     freelist + clear asleep in GBASIOLockstepDriverReset. (This edit was
     compile-broken for several runs — the "queue-clear didn't fix it" verdict
     was from a binary that never contained it.)
- **Server-side verification of the fixed stack: ROM+state load 200, no bootleg
  warnings, all 4 cores 60fps (old lib: sleep:[TTTT] freeze), identify OK,
  `FS kick: invoking` x32, `kicked deadlock (phase=2 recv=13 got12=1)` x3 —
  the kick now injects and games consume got12.**
- Rebuilt: WASM (10:54, into DualBoy/web + src), native lib + server (10:43,
  running as pid 91446), harness (10:44). build.sh dies silently when a
  concurrent build clobbers build-wasm/ — run make + emcc manually then.
- App stability: dualboy-web runs indefinitely via `setsid nohup ... &` (earlier
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
     injecting x2+). Browser retest pending the db_reset_sio + decline-diagnostics
     fix (round 4, below).

### 2026-09-09 (round 4) — browser P1 divergence: WASM import missing driver reset
- Root cause: `emulation.rs load_state_set` resets every driver after load;
  `dualboy_web.c db_load_state_bytes` (browser) did NOT — stale restored
  queues/asleep flags diverged P1's handshake (polled 0x0D000000, kick gates
  blocked, zero injections; server same file: 0 OOB + 219 injections).
- Fixes (rebuilt, server pid 119307): `db_reset_sio()` export + wired into both
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
5. [ ] Verify in the real app (dualboy-web / nav_fs.py + manual key events) if
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
- `DualBoy/tools/rendezvous.{c,h}` — mirror assist + `_fsAssistNormalize`
  (0xFFF1/0xFFF3 checksum-drift correction) + `ReadMultiRegs` + `SetFSArmed`.
- `DualBoy/tools/threaded_link.c` — `--fs3` (post-link flow, assist on),
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

## 🟠 DualBoy app / web remaining work

- [ ] Root-cause the one observed tokio-worker segfault (`segfault at 4a8` in
      `dualboy-web`): suspected cross-thread `load_rom` (tokio) vs `run_frame`
      (emulation thread). Needs sustained-play re-testing now that logging and
      the release build are fixed.
- [ ] Drive FS to *actual gameplay* at 4P and confirm the link-heavy title
      select stays smooth there.
- [ ] Audio routing — DONE for desktop ALSA; verify web audio parity and the
      per-player source menu end-to-end.
- [ ] Gamepad support for players 3–4 in the browser (Gamepad API — desktop
      webview works; check browser build).
- [ ] If WebView can't composite 30 FPS on low-end hardware: native
      (non-webview) renderer for the desktop app.
- [ ] Web version on GitHub Pages is live; keep it in sync with the desktop
      feature set (turbo, save states, audio source menu…).

## 🟡 Future dev options (documented, not built)

- **2×2 link groups** — two independent 2-player links (P1–P2, P3–P4) in one
  process, or arbitrary linked/independent core mixes.
- **Per-player screen toggle** — show one player's screen at a time (prereq
  for RetroAchievements-style play before any RetroArch port).
- **Pop-out windows** — each player's screen in its own OS window (multi-screen
  / streamer layouts). The threaded harness infrastructure (`threaded_link.c`)
  is a stepping stone here.

### 2026-09-10 (round 6) — the link-screen assist was never armed; state import is a dead end
- **Fixed:** `GBASIOLockstepCoordinatorSetFSArmed()` had exactly one caller in
  the tree — the test harness. Neither `emulation.rs` (desktop + web server)
  nor `dualboy_web.c`/`main.js` ever armed it, so `_fsAssistTick`'s discovery
  echo returned at its guard and was dead code in the app and the browser.
  Now self-armed in `_fsAssistKick` when the FS cart is identified and every
  attached player is in link-screen mode (IWRAM 0x6D10 == 9). Watch for
  `FS assist: armed at the link screen (all N players in mode 9)`.
  Caveat: 2P gameplay WAS reached in the harness with `armed=0`
  (2026-09-08 entry above), so the self-arm is a needed gap-fix, not proof by
  itself that the cooperative model completes the discovery.
- **The user's `dualboy.dualbystate` cannot reach gameplay from any model.**
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

## Housekeeping

- [ ] PROJECT_LOG.md is the long-form log; `history.md` is the tried-vs-next
      log. Keep both in sync at session boundaries.
- [ ] The `--fs*` experiment modes in `threaded_link.c` are dev tools; decide
      which to keep vs delete once the fix is found.
- [ ] Branch hygiene: `fs-link-loosen-timing` carries the uncommitted assist
      work + the `a0647ffac` timing-loosen cherry-pick. Plan a clean commit
      sequence once the fix is confirmed (the cherry-pick may be dropped if
      the assist makes it unnecessary).