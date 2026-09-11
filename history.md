# history.md — What we tried vs what to try next

> **This file exists so a future session can pick up without re-deriving dead
> branches. Append a dated entry after every experiment — even (especially)
> failures. Do NOT delete old entries; supersede them.**
> Long-form narrative lives in `PROJECT_LOG.md`; this file is the verdict
> index. Read the bottom-most entries first.

---

## The Four Swords link bug — one-paragraph summary

The FS linking screen never completes under stock mGBA. The two games exchange
FEFE probes and (value, 0xFFF1−value) checksum pairs whose internal counters
stay offset (the slave runs a round ahead), so neither game ever sees a
consistent partner and the discovery state machine loops forever. **Every
"plumbing" explanation has been ruled out** (below). The remaining attack
surface is the game's own acceptance logic, which we nudge with the **FS
assist** (driver-side echo/normalize) plus correct NORMAL-mode master read-back.

Reproduction (any session):
1. `cd mgba-splitscreen/src-tauri && ./target/release/mgba-splitscreen-web --players 2 > /tmp/mgba_splitscreen_web.log 2>&1 &`
2. POST the FS ROM to `http://127.0.0.1:8080/load_rom`
3. `python3 mgba-splitscreen/scripts/nav_fs.py --port 8080 --path /ws --players 2 --rom "Test Roms/..."` → FS title
4. START on both simultaneously → watch `/tmp/mgba_splitscreen_web.log` for `MULTI transfer finished`
   (or use the harness: `mgba-splitscreen/tools/threaded_link --fs3 "<rom>"`).

---

## Tried and verdicts (newest first)

### 2026-09-11 — Web deep links (`?players`, `?rom`) landed

Added `?players=1-4` (applied before `mgs_init`, so the coordinator is built at the
right size) and `?rom=<url>` (fetched, loaded into every core, cached into IndexedDB
and recorded in Recents so the launcher can relaunch it offline). `?players` was
verified by canvas count and the status line; `?rom` by booting Four Swords straight
to `CHOOSE A FILE` at both 2 and 4 players, all panels pixel-identical.

Failure paths were exercised rather than assumed: `?players=9` logs
`Ignoring ?players=9: expected a count from 1 to 4` and falls back to 2, and
`?rom=missing.gba` reports `Could not fetch ROM: HTTP 404 File not found —
check the ?rom= path`. Two distinct messages on purpose — an HTTP status means the
URL is wrong, while a rejected `fetch()` means CORS/network — because a shared link
fails for those two reasons about equally often and the old single message blamed
CORS for 404s.

`rom` is a plain `fetch`, so cross-origin URLs need permissive CORS on the host and
the browser will refuse a bare file; that constraint is now in the README and the
to-do rather than being discovered by a user.

### 2026-09-09 — ROOT CAUSE FOUND for the state-load freeze + browser corruption: GBASerialize garbage flags; FIXED

**The browser 4P session (user console + `/tmp/mgba-splitscreen_session.log`) cracked it:**

- Session log (1177 lines): `FS kick: invoking` ×11 (enabled=1) but **ZERO `kicked deadlock`** — the
  kick fired but every self-gate blocked (P1 corrupted). 192× `Out of bounds ROM Load32: 0x0D000000`
  (P1 executing garbage), 4× `Save state expects different bootleg type; not restoring bootleg state`.
- **Root cause of the corruption:** `GBASerialize` (src/gba/serialize.c) never zeroes the state and
  `GBAUnlCartSerialize` only writes `hw.unlCartFlags` for non-NONE bootleg carts. For a normal cart
  (FS 8MB, CRC 8E91CD13 == the ROM on disk) the field kept stale buffer bytes (0x6E29 → Type bits
  0-4 = 9 ≠ NONE) and `GBAUnlCartDeserialize` warned + **skipped the bootleg restore on every
  import**. Restoring a save set with the bootleg section missing left the games' memory partially
  garbage → P1's CPU jumped to 0x0D000000 and froze; P2-4 sat in linking-pairing mode.
- **Fix:** `memset(state, 0, sizeof(*state))` at the top of `GBASerialize`. Verified: fresh states
  have unlCartFlags=0. Also patched the existing `Test Roms/mgba-splitscreen.dualbystate` in place (zeroed the
  2-byte field at offset 0x2C2 in each of the 4 blobs; original backed up at
  `/tmp/mgba-splitscreen_orig.dualbystate`).
- **Second root cause — the state-load freeze (fr=0, hb=0, cores never run):** the driver reset left
  stale per-player event queues restored with the state (HARD_SYNCs in flight). The first HARD_SYNC
  each player processed immediately re-slept the secondaries, and with the games parked waiting for
  input nothing ever woke them. **Fix (in GBASIOLockstepDriverReset): drop the queue, rebuild the
  freelist, clear asleep/dataReceived, wake the user.** This edit sat compile-broken (undeclared `i`)
  for several fs11 runs — that's why the earlier "queue-clear didn't fix it" verdict was wrong; the
  binary never contained it. After fixing and rebuilding: **fs11j loads the state, all 4 cores run
  (fr=4088 in 60s, ~68fps), the link handshake completes, and all 4 games reach the linking screen**
  (was: total freeze).
- **Harness fs11j post-fix behavior (4P state load):** games run but the master's game never starts
  data transfers — only HARD_SYNCs (5103, master event ~35/s so the 8192 kick countdown never
  accumulates). Harness-specific: the **server-side** test below behaves like the browser instead.
- **Server-side verification (native lib, same code as WASM):** POST ROM + patched state → 200,
  **no bootleg warnings**, all 4 cores at 60.0 fps (old lib froze them: sleep:[TTTT]),
  `FS assist: Four Swords cart identified`, `FS kick: invoking` ×32, **`kicked deadlock
  (P1 phase=2 recv=13 got12=1, P2 phase=2 recv=13 got12=1)` ×3** — the kick now injects and the
  games consume got12.
- **Rebuilds done:** WASM (mgba-splitscreen-web.wasm/js → mgba-splitscreen/web + copied to mgba-splitscreen/src, 10:54),
  native lib + mgba-splitscreen-web server binary (10:43), harness relinked (10:44). Server restarted fresh
  (pid 91446). build.sh dies silently after the cmake make if a concurrent build clobbers build-wasm/
  (run `emmake make -C build-wasm` + the emcc link manually, as wasm_link.log shows).

### 2026-09-09 — browser 4P test: FREEZE at linking; audio bug found; harness extended to 4P + state loading
**User tested the rebuilt WASM in the browser at 4P: sessions freeze at the linking screen and never finish.**

**Browser console evidence (from the user):**
- Flood of `GBA Serial I/O: MULTI did not receive data. Are we running behind?`
  — stalled MULTI completions (the driver's stall-clear path is active; the games cycle).
- `FS kick: invoking (countdown=0 enabled=1 nAtt=4)` only a handful of times — the master's
  lockstep event barely runs, so the countdown almost never reaches 8192 (vs. 2P runs which cycled it).
- **Every frame:** `main.js:2379 Uncaught TypeError: First argument to DataView constructor must be
  an ArrayBuffer` at onAudio ← wasmPumpAudio. **Found & fixed:** `wasmPumpAudio` passed
  `onAudio(tagged.buffer)` (a raw ArrayBuffer) but `onAudio` does `new DataView(data.buffer, …)` —
  `data.buffer` is undefined on an ArrayBuffer. The socket path passes a Uint8Array and worked. The
  wasm path threw on EVERY frame: silent audio + a 60/s uncaught-exception console storm. Fixed by
  normalizing in `onAudio` (wrap non-Uint8Array in `new Uint8Array(data)`) and passing `tagged` from
  `wasmPumpAudio`. (Pre-existing committed bug, not from this branch's edits.)
- `GBA BIOS: Misaligned CpuSet source` — harmless (emscripten build of existing core warning).

**Session logging added (user asked “is it creating a log file?” — previously NO):** the in-browser
WASM engine writes nothing to disk; only the devtools console had it. Now `main.js` mirrors
console.log/warn/error into a ring buffer and POSTs new lines to `/session_log` every 2s; the Rust
server appends to `/tmp/mgba-splitscreen_session.log`. Also restarted `mgba-splitscreen-web` with the rebuilt binary
(has the endpoint). The audio fix + session log are in `mgba-splitscreen/src/main.js` + `web_server.rs`.

**Harness extended (threaded_link.c):**
- `--fs10 <rom> [n]`: the fs6 flow (real lockstep driver, inline kick, no harness kick) generalized
  from 2 to N players — every nav helper was already per-player, so it was mechanical.
- `--fs11 <rom> <dualstate> [n]`: loads a browser-exported `DUALSTATE` save-state set (the exact
  blob File → Export State Set produces: `DUALSTATE` + version u32 + count u32 + per-state len+blob)
  into N lockstep cores via `core->loadState`, mirrors the app's post-load driver reset, and runs the
  post-link watch from the saved position — **reproduces the browser freeze deterministically with
  zero menu navigation**. (User's 4P pre-link save state is the key input we still need.)

**fs10c run (--fs10, 4P, nav-driven): GAMEPLAY NOT REACHED (mode=9), ZERO kicks, ZERO MULTI traffic.**
- P1+P3 did reach the link/post-name phase machine (game=9:2 phase=1 recv=0/send=13, siocnt mode=1
  NORMAL-32) and sat there all 80s of watch — same 9:2 parking as the browser.
- **But P2+P4 never made it to the FS title** (pixel detector mis-navigation → they ended at the
  ALttP panel, mode 8/13). So the run is CONTAMINATED: 4 attached, only 2 participating.
- Striking observation: with 4 attached + 2 participating, probe traffic was ZERO (mlt=FFFF) and the
  kick countdown never advanced — vs. 2P runs which had FEFE probe traffic and a cycling countdown.
  The master appears to block on non-acking attached players, freezing the whole link.

**Working theory for the browser 4P freeze:** the 2P-validated fix (AckPlayer sleep removal +
round-boundary-gated kick) doesn't cover 4P: (a) if any of the 4 games isn't cleanly at the link
screen, the master blocks/cycles on it forever; (b) the FS-assist echo + kick table (slots 2-3 = FFFF
“no device”) were 2P-tuned and are not armed in the browser app anyway. Candidates to test once the
--fs11 repro exists: self-arm the assist at the link screen (mode-9 + lstat-engaged gates already
prove it), extend the kick's crafted table to 4 real device slots, and verify NORMAL-mode master
read-back with 4 players.

**NEXT: get the user's 4P pre-link save state exported (File → Export State Set) and run --fs11 to
reproduce the freeze headlessly. Then iterate on lockstep.c with full logs.**

### 2026-09-06 (continued — post-name phase machine, normalize+echo corruption FOUND & FIXED)
**The FS assist was corrupting valid game data — two real bugs found and fixed.**

**Bug 1 — `_fsAssistNormalize` rewrote game table entries.** Disassembly of
FS's SIO handler (checksum builder at 0x800C658, acceptance scan at
0x800C6A8) plus live traces prove the games' tables are SELF-CONSISTENT:
- Each game builds a 12-halfword block
  `[counter|A^B, checksum, 0x0020, 0xFFFF, 8 values]` where
  `checksum = ~(sum of other 11) − 14`, so the block sums to 0xFFF1 (−15)
  exactly (verified: freeze-time sendBufs `0029 FFA9 0020 FFFF` and
  `007C FF56 0020 FFFF` both sum to 0xFFF1). The acceptance scan requires
  block sum == −15.
- Every (value, next-value) pair the games exchange sums to EXACTLY 0xFFF2
  (FC01+03F1, FBF5+03FD, FEF3+00FF, FF39+00B9, ... — all d=1 from FFF1,
  one at d=513). These are consecutive TABLE ENTRIES (8 data values of the
  block), not (value, checksum) pairs. The prior session misread them as
  drifting pairs and the normalize "fixed" them (03C4→03C3, 03FF→01FE!)
  — destroying the block checksum → the master's game rejected every block.
- FIX: `_fsAssistNormalize` is now log-only (never rewrites data).

**Bug 2 — the echo overwrote real table data.** Observed fin
`data=FEFE FEFE FEFE FEFE` and `data=0000 0000 0000 0000`: the echo
replaced the slave's real value (00BE/00BF) with the master's value in all
slots, so the master's game never saw the slave's post-name table (this is
why the master stayed recv=0 while the slave advanced). FIX: the echo now
fires ONLY on FEFE probe rounds (`fsAssistOn && data[0] == 0xFEFE`) — the
probe exchange is what resets both games' sendIdx; real table rounds pass
through raw.

**Still stuck after both fixes** (link screen passes ✓, name screen reached ✓,
post-name device-recognition deadlocks). Current understanding of the freeze:
- Both games sit in the ROM phase machine (0x800C54C): phase=1, checksumA=0,
  sendIdx=13 (tables built but never re-sent), master recvIdx=0, slave
  recvIdx=13 (the INITIAL value — both games' recvIdx frozen).
- The master's game polls `[0x4000128]` (32-bit read → SIOCNT+SIOMLT_SEND)
  at ~18/s forever, seeing busy=0 val=FEFE, and NEVER WRITES SIOCNT again
  (last RAW SIOCNTW at the freeze; polls continue to the end). The phase-0
  completion (needs recvIdx==13 && SIOCNT&0x88==8 && !(SIOCNT&4)) never
  fires for the master.
- The EWRAM SIO handler (0x02030590, which advances recvIdx and stores
  received slots) runs at the post-link transition then STOPS being invoked
  (its SIOMULTI 32-bit reads vanish from the SIORD trace; only the ROM
  handler's 0x128 reads remain). It is likely the SIO IRQ handler; the
  games' SIO IRQ path post-name is the next thing to trace.
- Transfers DO keep completing until the very end (busy clears between
  polls; fins are count-gated every 25th so they just aren't logged), but
  the games exchange only zeros post-name (their idle tables; one 00BE/00BF
  and one FEFE each, once).
- No "Transfer restarted unexpectedly", "no secondary players", or
  "FS stall" WARNs — transferActive is NOT stuck; the driver is fine.

**SIO IRQ theory REFUTED (partially):** traced IE writes + SIO IRQ raises
(io.c IEW trace, sio.c SIOIRQ trace — both TEMP, revert before merge): the
games keep the SIO bit (0x2000) enabled in IE and SIO IRQs KEEP raising
post-name (169 logged after the post-link line, every 64th). So the games
get the IRQs, yet the EWRAM handler's SIOMULTI reads still stop — the EWRAM
handler is either not the SIO IRQ handler, or its invocation is
main-loop-driven and the main loop changed branches post-name.

**Freeze mechanism now FULLY decoded (caller loop at 0x800B120-1B0):**
- The game keeps a SHADOW SIOCNT at IWRAM 0x03000FF0. The main loop calls
  the phase machine (0x800C54C) via `bl` at 0x800B1A2 with r0=0x03000FF8
  (the scan's output area — NOT the EWRAM state!) and stores the built value
  to the shadow (0x800B1A6: `str r0, [r5]`, r5 = 0x03000FF0). The actual
  SIOCNT register write is a SEPARATE conditional step elsewhere; when the
  shadow stabilizes (built value stops changing), the register writes stop.
- At the freeze the built value is constant 0x600B (checksumA=0x60 << 8 |
  checksumB=0x0B; the MULTI/IRQ bits come from checksumA's own bits 4-6),
  so the game polls [0x4000128] (the phase machine's 32-bit read at
  0x800C552, pc+4=0x800C556 — the "0x12A polls") forever and never writes
  SIOCNT. This is a STABLE WAIT, not a crash.
- The wait is for the acceptance scan (0x800C6A8, called at 0x800C5FE)
  to find a received candidate block summing to -15 in the +44 recv
  histories — which are all zeros because both games' send tables are idle
  (sendIdx=13, sendBuf rebuilt to [counter, checksum, 0x0020, 0xFFFF, 0...]).
- The games only re-send their tables after a FEFE probe round resets
  sendIdx (EWRAM handler 0x20305D8-5DC: `ready=0; sendIdx=0` when slot0 ==
  FEFE). The master DOES send FEFE probes (~4× post-name, pc 0x800C7A2),
  the FEFE-only echo delivers them to the slave, but the slave's game never
  RESPONDS with FEFE (its FEFE sender at 0x800C75C is gated on state[0] !=
  0; both games' state[0] = 0), and the re-sent tables are the idle zeros
  anyway — the table REBUILDER (0x800C658) hasn't run again for the
  post-name phase, and its trigger/caller was not found (not in 0x8008000-
  0x800D000, not in EWRAM 0x02030000-0x02031000 — likely an indirect call).

**Next:** (1) find who calls 0x800C658 / the EWRAM-copy builder (search the
full ROM for its byte pattern or BL target; it may be reached via a function
pointer table — check the literal pools of 0x800C54C's neighbors); (2) the
post-name exchange needs the games to REBUILD their tables — on real
hardware the FEFE probe round-trip must trigger it, so trace what the master
expects in reply to its FEFE probe (it may need FEFE in slot1, not slot0);
(3) once tables flow, the self-consistent blocks should pass the -15 scan
and the driver delivery path is verified good.

### 2026-09-06 (this session so far — FS post-link deadlock, mode + data-collection findings)
**The games never leave MULTI mode — the old "NORMAL32 post-name handshake"
premise was wrong.** GBATEK: SIOCNT bits 12-13 select the mode (01=NORMAL,
10=MULTI); the harness's `(mode=siocnt>>14)` was a red herring (bit 14 is the
IRQ enable). The games write SIOCNT 0x600B/0x601F = valid MULTI values, and
the io.c SIORD trace (gated on `sio.mode == GBA_SIO_MULTI`) fired 8,814×
through the post-name window, so mGBA stayed in MULTI the whole run. Mode
history from the driver: GPIO → NORMAL8 → MULTI, never leaves MULTI. The
NORMAL read-back fix in FinishNormal8/32 was aimed at a mode FS never uses
(keep it anyway — correct hardware behavior).

**Post-name freeze state (fs3 run):** master `send=13 recv=0`, slave
`send=13 recv=13` (FS state-struct counters). Master wrote SIOCNT 0x608B
(MULTI+IRQ+busy=start), slave 0x601F — then NO transfer ever completed.
Master reads SIOMULTI1 once (val 0000 — dead data), then BOTH games fall
into a tight 16-bit poll of SIOMLT_SEND (0x12A): master polls FEFE, slave
polls 00BE — each reads back its OWN outgoing value forever.

**The smoking gun — data collection:** 379 of ~430 `MULTI fin` transfers
collected `0000 0000 FFFF FFFF` (both players' outgoing data = 0) even
though the games demonstrably wrote FEFE/00BE to 0x12A (the polls read those
values back). The game's data write isn't landing in `memory.io[0x12A>>1]`
before the driver's `_setData` collects it.

**Prime suspect — 32-bit writes to 0x128:** `GBAIOWrite32`'s default case
splits lo-half FIRST (`GBAIOWrite(addr, value&0xFFFF)` then
`GBAIOWrite(addr|2, value>>16)`). A 32-bit STR to 0x4000128 therefore writes
SIOCNT (fires the transfer via busy bit, collecting stale SIOMLT_SEND) BEFORE
the SIOMLT_SEND half lands. Game then polls 0x12A and sees its own data
(FEFE/00BE) — matching the trace exactly. If FS does `STR rN,[0x4000128]`,
that's the bug. (mGBA issue #3286 territory.)

**Verified plumbing (not the bug):** 32-bit IO reads bypass GBAIORead (raw
`memory.io` read, memory.c) — the traced 0x12A polls are genuine 16-bit
reads. GBATEK: SIOMLT_SEND is purely outgoing; received data lands in
SIOMULTI0-3 after a MULTI transfer. `GBAIORendezvousDriverStart` memsets
multiData to 0xFF then `_setData(0)` reads memory.io[SIOMLT_SEND]; the
secondary's data is read at the TRANSFER_START event. linktest (works)
writes SIOMLT_SEND then SIOCNT as separate 16-bit writes.

**Next:** (1) confirm FS's write pattern — add width-aware SIO write trace
(GBAIOWrite32 when addr==0x128 + SIOMLT_SEND writes) or disassemble FS's SIO
handler; (2) if 32-bit: fix = handle addr 0x128 in GBAIOWrite32 by writing
SIOMLT_SEND before SIOCNT (hi-first), i.e. mirror real hardware where the
data half is visible before the start bit; (3) also check whether mGBA should
write received data back into SIOMLT_SEND after MULTI transfers (FS polls
0x12A expecting it to change — real-hardware shift-register behavior).

### 2026-09-07 (this session — EWRAM handler + acceptance scan fully decoded, kick gets games to phase-2)
**The games' own SIO protocol is now FULLY decoded (three EWRAM functions + the
ROM phase machine + the acceptance scan), and a memory-injection "kick" gets
both games to phase=2 with mutually-validated blocks. The last unknown is the
phase-2 → gameplay transition.**

**EWRAM SIO handler (0x02030590, copied from ROM 0x800C7E8; this is the SIO
IRQ service — prologue writes 0 to IME 0x4000208, epilogue restores):**
operates on the state struct at EWRAM 0x02030790 (`r4` = state base;
literals: `+0x20`=SIOCNT 0x4000128, `+0xec`=0xFEFE, `+0xf4`=IWRAM shadow
0x030007F8, `+0xf0`=0x4000208 IME). State layout: `+0`=role byte,
`+1`=phase byte, `+2`=acc-accepted, `+3`=scan-accepted, `+4`=ready,
`+5`=got12, `+7`=busy byte, `+8`=round, `+9`, `+11`=cnt, `+20`=sendIdx,
`+24`=recvIdx, `+28`=sendPtr, `+32`=sendPtr2, `+36`=recvBufB,
`+40`=recvBufA, `+44`=recvBufC.
- **Probe / FEFE round** (if received SIOMULTI0 == 0xFEFE AND recvIdx > 11):
  recvIdx = −1, swap recvBufB↔recvBufA (+36↔+40) AND sendPtr↔sendPtr2
  (+28↔+32, only if `+4` set), sendIdx = 0, write 0 to 0x4000208, set
  SIOCNT busy |= 0x80 and start a new transfer. So a completed block in
  recvBufB moves to recvBufA on the next probe.
- **Send:** if sendIdx <= 11, write `sendPtr[sendIdx]` to SIOMLT_SEND (0x12A);
  sendIdx++ (cap 13).
- **Receive/store:** if recvIdx >= 0, store the 4 received halfwords (sp
  loaded from 0x4000120/0x4000124) into recvBufB at `recvIdx`, 4 slots at a
  28-byte stride; if recvIdx == 11 set got12=1 (`+5`); recvIdx++ (cap 12).
- **Slave re-start:** if role != 0 and sendIdx <= 12, also set busy and write
  0xC0 to IME. (The MASTER drives via the ROM phase machine; the slave
  self-starts here.)
So each player accumulates a **12-halfword block per slot** in recvBufB over
12 transfers, and got12 is set after the 12th.

**Acceptance scan (ROM 0x800C6A8, called by the phase machine at phase 2):**
calls the gate trampoline (ROM 0x129940 → `bx r0` → EWRAM 0x02030950); if the
gate returns nonzero (got12), for each slot 0..3 read 12 halfwords at
`state+44 + 28*slot`, sum them, and if sum == −15 (0xFFF1) accept: copy the
block out (CpuSet to `r9 + 20*slot`) and set bit `1<<slot` in `state[3]`.
Finally `state[2] |= state[3]`. Slots for players 0-1 must each sum to −15;
slots 2-3 are all-FFFF for 2P (sum FFF4, correctly rejected).

**Gate (EWRAM 0x02030950, ARM):** returns got12 and **swaps state+40 ↔
state+44** on each call (toggles 0x4000208 IME). Buffer rotation therefore is
handler-write → recvBufB(+36) → [probe swap] → recvBufA(+40) → [gate swap] →
recvBufC(+44) → acceptance scan reads. The scan ALWAYS reads +44 after the
gate swaps +40↔+44, so a freshly-completed block in +36 must survive TWO
swaps before it is validated.

**Kick (`fs_kick_deadlock` in threaded_link.c, armed by the harness watch
loop t∈[24,90)):** the games naturally build valid −15 blocks in recvBufB but
never rotate them to +44 for validation, so the kick writes the current
master table (identical `[counter, checksum, 0x0020, 0×9]`, sum −15) into
slots 0-1 of BOTH +40 and +44 of each game (slots 2-3 = FFFF), sets got12=1,
master recvIdx=13, both sendIdx=0. Since +40 and +44 both carry valid data,
the gate's swap can't lose it.

**Results (relaxed kick, idempotent every-loop):**
- `--fs3` (echo ON + kick): games now reach **phase=2 role=8/0 a=3 b=3**
  (blocks accepted!), then **cycle phase 1↔2** forever — the echo causes the
  phase resets. Screen stays "name".
- `--fs4` (echo OFF + kick): games reach **phase=2 role=8/0 a=3 got12=1
  send=13 recv=12** and PARK stably (no more cycling), with the EWRAM handler
  still running idle-0000 transfer rounds forever. recvIdx parks at **12**
  (probe sets −1, increments cap at 12; got12 set at 11), so the ROM phase
  machine's phase-0 completion check `recvIdx==13` is **unreachable
  naturally**, but the master already completed phase 0 earlier (role=8).
  Both games sit in phase 2 with mutually-validated blocks and never leave.
- **The echo is confirmed harmful post-name** — it makes the games reset
  phase 1↔2 instead of parking (fs3q/fs3r vs fs4r). Keep the echo for the
  LINK SCREEN, disable it for post-name. (The kick alone, echo off, is the
  best state so far.)

**Remaining blocker (THE question): what makes the games leave phase 2?** The
phase machine's phase-2 path (0x800C54C+0xb0) calls the acceptance scan then
builds/returns a SIOCNT value; it does NOT itself advance the phase. Both
games validate (a=3) but stay phase=2 with idle-0000 transfers. Candidates to
investigate next: (1) the phase-2→phase-3 condition lives in the main loop
(0x800B040-0x800B2CC) or the table-builder (0x800C658) and depends on a
received-block COUNTER matching the current round (the kick injects the
current counter, but the games' expected counter may differ); (2) a minimum
number of consecutive validated rounds; (3) the games may be waiting for a
button press or the name-screen confirm to complete. Disassemble the main
loop's phase dispatch (the `bl 0x800C54C` caller) to find the phase-2 exit.

### 2026-08-25-ish (last session, UNLOGGED — reconstructed from the working tree)
**What:** Built the **FS assist** into both `lockstep.c` and `rendezvous.c`:
lazy cart ID from ROM title "GBAZELDA"; host-armed (`SetFSArmed`) engagement;
echo the primary's sent value into every slot once 3 consecutive discovery
rounds are seen; echo on stalled (no-ack) rounds + clear stuck
`transferActive`; hand off to raw data after 120 quiet rounds. Plus a
**NORMAL8/32 master read-back fix** (upstream `FinishNormal8/32` never gives
the master the slave's data — the master reads back FFFF/no data, so the
"post-name" NORMAL-mode handshake deadlocks with the master stuck at recv=0
while the slave advances to recv=13). Plus `_fsAssistNormalize` in
rendezvous.c: the post-name (0xFFF3) checksum intermittently drifts by exactly
0x100, so snap near-target pairs to the exact target. New harness modes:
`--fs3` (full post-link flow with assist), `--fs4` (no assist), `--fs5`
(no assist + full SIO DEBUG to capture raw post-name transfer values).
**Result:** NOT RECORDED — the machine was rebooted and /tmp logs were lost
before anyone wrote the results. This is the current tree state; re-running
`--fs3`/`--fs4` is the immediate next step.

### 2026-08-18 — P-series investigation (PROJECT_LOG "Ordered path forward")
- **P1** (trace slave's SIOMULTI reads + response branch): superseded by the
  assist approach; partially done via io.c read trace.
- **P2** — **Shining Soul II 2P: PASS.** 399s live Tauri session, 0
  WARN/ERROR, 60.0 fps both, healthy value+complement pairs (constant 0xB0A6)
  with counters advancing in lockstep. FS is a game-specific protocol issue,
  NOT a driver-wide bug.
- **P3** (gbatek SIOMULTI semantics): not done as a separate step; folded into
  the NORMAL-mode read-back fix.
- **P4** (value-convergence experiment): not done; the echo assist is its
  descendant.
- **P5** (driver fix + regression): pending.

### 2026-08-18 — SIO IRQ / halt timing (H6): **REFUTED**
Traced `GBARaiseIRQ(GBA_IRQ_SIO)` vs transfer completion on the slave:
SIO IRQ fires in the same timing tick the transfer finishes (delta p50 = 0),
halt is released exactly GBA_IRQ_DELAY (7) cycles later, 55k TRIG events with
`wasHalted=1`. Slave IS in IntrWait with SIO IE enabled. Interrupt delivery is
flawless. Handshake pairs are internally valid both ways (`0088+FF69=FFF1`).
The games still never accept the link.

### 2026-08-18 — Busy-bit boundary instrumentation (H4): **REFUTED**
Slave's busy-bit window (clear−set) median 5,755 cycles = the full nominal
GBASIOCyclesPerTransfer[baud3][1]; 0/94,601 windows < 5,000. The busy bit is
fully observable when the game polls. Sharper finding: the **master reads
SIOCNT 7,635 times; the slave reads it only 11 times** — the slave is not in a
busy-poll loop at all; it writes SIOMLT_SEND (86,904×) and cycles SIOCNT baud
0↔3, waiting *long* stretches between attempts. Led to H6 (refuted above).

### 2026-08-18 — Baud-mismatch corruption hypothesis: **NEGATIVE**
Simulated MULTI baud-mismatch all-ones corruption: never fired (slave's baud
is always 3 at every transfer completion). Slave's baud-0 writes happen
outside transfer bursts. Not the bug.

### 2026-08-18 — Ground-truth asymmetry (the key clue)
Master P0 writes SIOCNT `208B` (baud 3, busy=1) 94,614× and reads busy≈50% of
the time. Slave P1 writes `601F`/`201C` (baud 3/0, busy=0) only 184×, reads
busy=1 only 5/194 times. The games are in **different handshake states**:
master probing (FEFE+busy), slave stuck in baud negotiation, nearly never
observing a transfer in progress. This is the concrete manifestation of the
value-counter offset.

### 2026-08-18 — Rendezvous (cycle-locked) driver: **FS still fails → NOT cycle drift**
Built `GBASIORendezvousDriver` (no per-transfer hard sync, no post-ack
secondary sleep → both sides complete at the same shared cycle). 4-player
linktest PASS with measurable near-zero drift; FS still cycles FEFE forever
with value counters offset by 13→19. **The failure survives tight
cycle-lockstep** → not a data race, not frame/cycle drift.

### 2026-08-18 — Threaded harness (H3): **Threaded execution does NOT fix FS**
Built `mgba-splitscreen/tools/threaded_link.c` (real threads, real blocking
deferred sleep/wake, 60fps pacing). 4-player linktest: 0 drops. FS: 95,056
transfers, 0 drops, but **7,341 FEFE probes at ~120/s for 60s — handshake
never completes**, identical to the sequential wrapper. **Bug is in
`lockstep.c` (upstream #3286), not the mgba-splitscreen single-threaded wrapper.**
Retired the "port to mCoreThread" idea as a fix.

### 2026-08-18 — H1 (per-transfer hard sync too aggressive): **partial, not the fix**
Removed per-transfer `_hardSync` from `finishMultiplayer`: linktest PASS,
FS handshake desync REDUCED (off-by-one + missed rounds mostly gone; games
exchange MATCHING value+checksum pairs) — but the handshake still cycles
forever. Completion failure is NOT the off-by-one desync.

### 2026-08-17 — FS handshake root-cause (no fix)
Data path correct (12,741 transfers, every `MULTI transfer finished` value
matches what each side wrote; no 4-bit/16-bit issue). Game reads SIOCNT only
(zero RCNT reads). Our C SIO code is upstream-identical modulo the
non-positive-delay clamp. **Upstream mGBA issue #3286** (still open,
`blocked: needs retest`).

### 2026-08-17 — `a0647ffac` "Loosen timing" re-applied (branch fs-link-loosen-timing)
Cherry-picked upstream's reverted experiment (UNLOCKED_INTERVAL 4096→8192;
hard sync only when `!waiting`; `nextHardSync` reset in AckPlayer).
4-player linktest: PASS, no regressions. FS 2P: **materially improved** — the
linking screen now passes into character-select (was: hung forever), then
stalls on the post-link screen. Kept on the branch (best-known state, safe),
but not a complete fix.

### 2026-08-16/17 — Cooperative sleep / lockstep crawl fixes (DONE, kept)
Sequential-wrapper sleep semantics, primary-only early frame end, cooperative
`runLoop` stepping (280,896-cycle budget, skip sleeping players), clamp
non-positive sync delay. Fixed: "MULTI did not receive data" power-off screen,
half-speed primary, 128s cycle-wrap crash. **These are the baseline that made
the FS work possible — do not regress them.**

---

### 2026-09-07 (night) — 🏆 WORKING MULTIPLAYER: games reach mode 2:9 (gameplay) and respond to input!

**THE FIX WORKS.** With `--fs4` (echo assist OFF) + the kick active from t=2
+ A-taps every 10s via a wall-clock accumulator (not `t % 10`), BOTH runs
reproducibly walk the full chain:

    mode 9:2 (link screen) → mode 5:4 (char-select) → mode 2:8 → **mode 2:9
    (game session) at t=35, stable through t=90+**

Screen captures at t=90 (fs4z + fs4aa, both reproduced) show **real gameplay**:
two Links (green P1, red P2) side-by-side, 7 heart containers, player badge,
shield icon, rupee counter, enemies, trees/grass/bridges. The pixel detector
labels it "alttp" (bright-green gameplay misread) — memory `game=2:9` is the
source of truth.

**Gameplay input probe (fs4ab):** after reaching mode 2, the harness drove
RIGHT/DOWN/LEFT/UP/A on BOTH players for 20s:
- Frames advanced 12303→13433 (~54 FPS, no freeze, no desync)
- Transfers kept flowing (BUSYSET/BUSYCLR/FEFE rounds during gameplay)
- Screen diffs 44-78% during input bursts, ~0% when idle → characters MOVE
- t=13s capture: sprites moved AND are holding swords (attacking) → the
  game session is fully interactive with live link

**What makes it work (the complete recipe):**
1. **Kick V2** (`fs_kick_deadlock`, fires every loop from t=2): writes the
   crafted −15 block `[0x00B0, 0xFF21, 0x0021, 0xFFFF, 0×8]` into BOTH recv
   buffers (state+40 AND state+44, because the gate at 0x02030950 swaps them
   on every scan), sets got12=1, recvIdx=13 (master), sendIdx=0 → the mode-9
   acceptance scan validates slots 0-1 (a=3), the link-status machine hits
   state 5, `[0x03000FC3] |= 0x40`, and the char-select gate
   ([0x03000FC8]==2) opens.
2. **A-taps every 10s on both players** (wall-clock accumulator). The FIRST
   A-tap (t=20) confirms the mode-9 link flow → char-select; the SECOND
   (t=30) confirms char-select → mode 2 (game session). Without the taps the
   games sit at char-select forever (fs4x/fs4y proved the old `t % 10` bug:
   taps only ever fired at t=20/30).
3. **Echo assist OFF** (`--fs4`, `armAssist=false`): the FEFE echo is
   unnecessary once the kick provides valid blocks; fs3r showed echo+ kick
   cycles phase 1↔2 forever, fs4r/z/aa with echo off park cleanly.
4. **Kick from t=2** (was t=24): prevents the driver hard-freeze that
   sometimes stalls both cores during mode 9 before the assist can engage
   (fs4x froze at 16s pre-kick).

Note: the mode-9→5 transition needs the A-tap AND kick together; the
t=20/30 taps are exactly one loop apart (~10s). The state machine ignores
premature presses (sub-state 4 A-check is gated on the link-status counter),
so spamming is safe.

---

### 2026-09-07 (continued) — LINK SCREEN BEAT: A-tap reaches char-select (mode 5)!
**The kick V2 (crafted 0x21 block) + periodic A-taps BROKE THROUGH mode 9.**

Chain of discoveries this session:
1. **Kick V2 injects a crafted valid block with 0x0021 at column 2** (sum
   still −15): the accepted-payload check `0x8037b5c(0x21)` requires each
   accepted slot's payload at 0x03000FF8+20*slot to start with byte 0x21
   (the "link active" marker). The idle `0x0020` blocks failed it; the
   crafted `0x0021` blocks pass → link-status machine hits state 5,
   `[0x03000FC3] |= 0x40`, sub-state-4 gate passes.
2. **Mode-9 sub-state machine fully decoded** (dispatch table 0x084278C0,
   sub-state byte [0x03000BFC]): sub-state 3 waits for A-press to start
   linking, sub-state 4 runs the link + waits for a second A-press to
   confirm, sub-state 5+ check the 0xF0-masked player-ready pattern.
3. **The harness input probe was DISABLED** (`(void) nextProbe; (void)
   probeNo;`) — the games sat at the A-wait the whole time. Re-enabled
   periodic A-taps on both players → games advanced into sub-state 4
   (linking) and then — with kick V2 — LEFT mode 9 entirely.
4. **Games reached mode 5:4 = CHARACTER SELECT.** Pixel detector calls it
   "title" (bright screen misread) — memory says game=5:4, and mode 5's
   dispatch table (0x0842731C) maps mode 5 → 0x803006d (char-select
   handler). Sub-state 4 (0x80304fc) handles Up/Down cursor + A/B confirm,
   gated on `0x8037554()` == ([0x03000FC8] == 2). The link-status struct
   at 0x03000FC0: +1 state, +2 counter, +3 status (bit 0x40 = success),
   +8 = gate counter. In the fs4w run lstat=05,01,15,43 → state=1,
   counter=21, status=0x43 (0x40 set + slots 0-1), lstat8=02 → gate OPEN.
5. **THE REASON A-TAPS STOPPED WORKING: `t % 10 == 0` timing bug.** The
   watch loop's `t` skips around (screen polling is slow, t lands on
   33,35,37,39,41...) so `t % 10 == 0` fires only at t=20 and t=30 — after
   that NEVER again. The games have been sitting at char-select waiting for
   an A-press that never comes. FIXED: wall-clock accumulator
   (`now_ms() - lastTap >= 10000`). This is likely THE final unlock —
   char-select's A-press needs the gate open (it is) + a tap (now reliable).
6. **Sub-state 5 (0x80305bc)** reads the player's chosen char index
   ([r2+8] → char table 0x08427938), writes [0x03000450+1]=char and
   +2=isFirst(0/1), then `0x800b5ec(3)` if char==2 && count>1 else
   `0x800b5ec(2)` — the "advance/continue" path.

**Next experiment: re-run fs4 with the fixed tap timer.** Expect char-select
sub-state 4 → 5 on the first A-tap after t=30.

---

## What to try next (ordered)

**✅ THE FIX IS DONE (harness-level): `--fs4` + kick from t=2 + A-taps every
10s reproducibly reaches mode 2:9 = PLAYABLE multiplayer (characters move,
link alive, no freeze through t=90+ and a 20s input probe).**

Remaining work is PRODUCTIZATION, not debugging:
1. **Port the kick + tap recipe into the real app.** The harness
   (threaded_link.c) proves the mechanism; the mgba-splitscreen desktop app needs it:
   - the kick is harness-only today (writes via mCoreGetMemoryBlock into
     EWRAM 0x02030790 state); decide whether it lives in the app's watch
     loop, the rendezvous driver, or a new FS-gated assist module
   - the A-tap needs a UI path (auto-press A on both players when the app
     detects the link screen; the desktop app already has frozen-link-screen
     detection that arms the assist — extend it to send the two A-taps)
2. **Regression-gate the driver changes** (sio.c, io.c, rendezvous.c,
     lockstep.c diffs): linktest 2P/3P/4P FRM parity, 128s wrap, other 2P
     games — before shipping the assist in the app.
3. **Decide the endgame** (PROJECT_LOG 2026-08-18): keep the kick+echo as
   FS-gated + screen-armed assist, or fold into a bespoke deterministic
   driver (the rendezvous driver already proves cycle-lockstep).
4. **Clean up TEMP instrumentation** (RAW SIOCNTW, busywrite, fincore logs)
   and decide which traces stay at WARN vs DEBUG.

1. **Find the phase-2 → gameplay condition.** Disassemble the main loop's
   phase dispatch (caller of `bl 0x800C54C`) and the table-builder
   (0x800C658) to see what makes the games leave phase 2. Leading candidates:
   (a) the received-block COUNTER must match the current round (kick injects
   the current counter but the games' expected counter may differ — try
   injecting the counter the game is about to expect, or leave recvIdx at a
   value that makes the game re-check); (b) a min count of consecutive valid
   rounds; (c) a button press / name-screen confirm the harness isn't
   sending. Also confirm the screen label — is "name" actually the linking
   screen, and does the game need input to proceed?
2. **Try forcing the phase directly.** Since both games already have valid
   blocks (a=3) at phase 2, extend the kick to also set `state[1]` (phase) to
   the post-phase-2 value (and/or role for the slave) once both have a=3 — a
   heavier hand than recvIdx=13. Observe whether the games proceed to
   char-select/gameplay.
3. **Disable the echo for post-name permanently.** The echo is confirmed to
   cause phase 1↔2 cycling (fs3q/fs3r) vs stable parking with echo off
   (fs4r). Keep it for the LINK SCREEN only.
4. **Once FS reaches gameplay:** regression-gate everything (linktest FRM
   parity, 128s wrap, other 2P games), then decide the endgame: keep the
   kick+echo (FS-gated + host-armed), narrow it, or turn it into a PR to
   upstream mGBA.
5. **Fallback: bespoke deterministic driver as the actual fix** — the
   rendezvous driver already proves cycle-lockstep works for linktest; if the
   assist is too hacky for upstream, the "rewrite the link logic" path is
   still open (PROJECT_LOG 2026-08-18).

---

## Reusable artifacts / gotchas (don't rediscover these)

- **Instrumentation toggles:** `src/gba/io.c` currently has TEMP read-trace +
  RAW SIOCNTW logs; `src/gba/gba.c` had SIO IRQ/halt trace (reverted). The
  `BUSYSET`/`BUSYCLR` logs in rendezvous.c are cheap and can stay.
- **Log volume:** full `gba.sio` DEBUG = gigabytes per FS run; throttle with
  count-gating or use `--fs5` only when you need the raw stream.
- **Rebuild after touching C:** `cmake --build <out>/build` (via cargo) then
  `mgba-splitscreen/tools/build.sh`; build.sh now picks the newest OUT_DIR.
- **Linktest first:** any suspected wrapper/lockstep regression — FRM parity
  (all counters must advance at the same rate), STALL=0, no 128s crash.
  `mgba-splitscreen/linktest/linktest.gba`, baud 3, `--players 2|3|4`.
- **FS value signatures:** link screen checksum target 0xFFF1; post-name
  handshake target 0xFFF3; slave checksum drift by exactly 0x100; slave
  counter drift by ~0x100 or ~0x13 depending on phase. FEFE = probe,
  FFFF = "no device present" (from unacked transfer), 0000 = idle.
- **Screens are OCR-unfriendly:** FS's bright title/char-select screens
  misread as "alttp"/"stuck" — use pixel-diff + screen-name state machines
  (nav_fs.py, threaded_link.c `cur_screen`), not OCR.
---

### 2026-09-08 — fs12 (lockstep, lstat gate): title-kick fixed, payload check still fails

**Change tested: gate `_fsAssistKick` on the link-status machine being ENGAGED**
(lstat state byte at 0x03000FC1 != 0 OR sub-state 0x03000BFC != 0). Rationale: the
FS title screen runs the SAME phase machine (phase=2 role=8/0 a=3 b=3 got12=1) for
link detection, so the old mode-9 + phase-signature gates fired the kick from boot
and corrupted the pre-link state. The harness pre-START probe proved the title is
distinguished by lstat all-zeros + ssub=0 vs post-name lstat engaged + ssub=4.

**Result (fs12, --fs6 = lockstep + driver kick + auto A-taps):**
- Kick now injects ONLY 7× post-name (was firing from boot). ✅ gate works.
- Games validate after each kick (a=3 b=3, got12=1) but the round STILL times out
  ~2s later (phase=1 round=4) — same cycling as fs9/fs10.
- **Streams are FIXED**: fs12 now exchanges synchronized real pairs
  (0037/0037, FF9A/FF9A, 0021/0021 markers — the fs4z signature). Previously
  fs9/fs10 only ever exchanged offset discovery pairs. The title-kick WAS the
  corruption; the gate cleaned the streams.
- **BUT the games never latch**: lstat stays state=3 (counter counts down
  15→0 then resets) — the accepted-payload check (0x8037b5c, wants 0x21) never
  passes under lockstep. fs11 (rendezvous SUCCESS) hits lstat=02,05,15,43
  (state=5, status=0x43) every other sample from t=4s and LATCHES at t=26s,
  then advances to 05,01,15,43 (char-select gate open).
- fs12's lstat=02,05,15,43 appears ONCE at t=74s but falls back.

**Remaining question**: why does the payload check pass under rendezvous but not
under lockstep, when the drivers are structurally identical? Next: disassemble
0x8036b58/0x8037b5c (link-status state machine + payload check) to see exactly
what gates state 3 → state 5.

### 2026-09-08 (mid) — fs7 (lockstep + HARNESS kick): decisive isolation — the lockstep DRIVER is the problem, not the kick
**Experiment**: --fs7 = app's real lockstep driver + the KNOWN-GOOD harness kick
(the exact fs_kick_deadlock that reaches gameplay on rendezvous in fs4/fs11) +
auto A-taps. Only variable vs fs11 (SUCCESS, rendezvous+harness kick) is the
driver: rendezvous.c vs lockstep.c.

**Result (fs7): FAIL, identical signature to fs12.** Full 90s watch:
- Harness kick fired every loop from t=2 (FS KICK lines present, ~45 fires).
- Games validated after each kick (phase=2 a=3 b=3 got12=1) and lstat hit the
  success signature `02,05,15,43` (state 5 / 0x40 bit) **34 times** — but
  ALWAYS fell back to phase=1 round=4 timeout ~2s later.
- `game=2:9` count = **0**. Final: "GAMEPLAY NOT REACHED (mode=9)".
- 187,696 MULTI transfers completed; zero stalls ("did not receive" = 0).

**Conclusion: the lockstep driver itself (its transfer-completion / round
pacing) differs behaviorally from the rendezvous driver, even though the two
files diff as structurally identical.** fs11 kicks at recv=12 and locks;
fs12/fs7 (lockstep) games stall mid-round (recv=3-8) and every round times out
at round=4. The kick is necessary but NOT sufficient on lockstep — the games'
OWN 12-transfer rounds must complete naturally under the driver's timing, and
under lockstep they never do.

Next: find the driver-level difference that makes the games' own rounds
complete under rendezvous but time out under lockstep. Candidates:
(a) secondary completion is scheduled at the SAME local cycle under rendezvous
but LATE under lockstep (the SIO IRQ / EWRAM-handler delivery timing);
(b) the games' busy-bit window / poll behavior differs because of how each
driver clears busy relative to the shared clock;
(c) hard-sync placement (HARD_SYNC_INTERVAL 0x80000) stalls the secondary
mid-round under lockstep.

### 2026-09-08 (mid) — fs7b (lockstep + harness kick, INLINE KICK DISABLED): **GAMEPLAY REACHED ON THE LOCKSTEP DRIVER**
**The inline driver kick in lockstep.c is ACTIVELY HARMFUL. With it disabled,
the app's real lockstep driver + the harness kick reaches mode 2:9 exactly like
the rendezvous driver (fs4/fs11).**

Two runs, identical harness (lockstep driver, auto A-taps, harness kick fires
every loop from t=2):
- **fs7 (inline kick ENABLED): FAIL** — game stuck at 9:2 for all 90s. Inline
  kick fired 56× (7 logged, every 8th) at **mid-round states** (P1 phase=2
  recv=7/8/5/4/9 got12=1). Games cycled phase 2->1 every ~2s forever.
- **fs7b (inline kick DISABLED via new fsKickEnabled kill-switch): SUCCESS** —
  game=9:2 -> 5:4 (char select) at t=33 -> 2:8 at t=44 -> **2:9 (gameplay) at
  t=46**, stable through t=90. Harness kick fired 41× mostly at ROUND
  BOUNDARIES (P1 phase=2 recv=12 got12=1 x31, phase=1 recv=0 x7). Payload
  check walks 0x21->0x50, lstat latches state 5 + 0x40, char-select gate opens.

**Why the inline kick harms (hypothesis, high confidence):** both kicks write
the same crafted block, but the inline kick fires from INSIDE the master's
`_lockstepEvent` timing callback (every 8192 events ~2s emulated, mid-transfer-
stream) at whatever state the games happen to be in — including mid-round
recv=4-9 where the game is actively accumulating its own 12-transfer round.
Injecting got12=1/recvIdx=13/got12 there makes the acceptance scan validate a
premature half-round and desyncs the game's own round accounting -> every
natural round times out (phase 1, round=4). The harness kick fires from the
host watch thread every ~2s wall, which empirically lands at round boundaries
(recv=0 deadlocked or recv=12 complete) where the injection is safe. In the
threaded harness, the inline kick also writes P2's EWRAM from the master's
thread — a cross-thread data race on the running P2 core.

**Conclusion: the driver-side inline kick (as built) must not fire. The kick
belongs in the HOST layer** (app watch loop / harness), like the proven fs4
recipe, OR the inline kick needs to fire only at genuine round-boundary stalls
(recv==0 or recv>=12), never mid-round.

Next steps:
1. Decide kick placement for the real app: host-side (app already has frozen-
   link-screen detection that can run fs_kick_deadlock-equivalent + A-taps) or
   an inline kick re-gated to round boundaries only.
2. Confirm fs7b's stability with the gameplay probe (it ran the 20s probe:
   frames advanced, no freeze) and ideally re-run for reproducibility.
3. Clean up: keep the fsKickEnabled kill-switch (useful), decide the --fs7
   mode's fate, update to-do.md.

### 2026-09-08 (mid) — ROOT CAUSE FOUND: the driver-side inline kick fires MID-ROUND and desyncs the games
**fs7b result (above) proves the lockstep driver reaches gameplay when the
harness kick fires at ROUND BOUNDARIES (recv==0 deadlocked, recv==12 parked,
or recv==-1 probe) — but fails when the inline driver kick additionally fires
MID-ROUND (recv=4-9). Both kicks write identical bytes; the difference is
WHEN they fire.** The harness kick (host thread, ~2s wall) samples the games
at whatever state they're parked in between its sleeps; the inline kick fires
from the master's `_lockstepEvent` at a fixed emulated cadence that lands
mid-round while the game is actively accumulating its own 12-transfer round.

**FIX (tested as fs8, --fs6 inline-only with new gate):** `_fsAssistKick` now
requires recvIdx to be at a round boundary on BOTH players — recv==0,
recv>=12, or recv==0xFFFFFFFF (probe reset) — and returns early if ANY player
is mid-round (recv 1..11). Rationale: mid-round the game is progressing on its
own; injecting got12=1/sendIdx=0/recvIdx=13 there makes the acceptance scan
validate a premature half-round and every natural round times out.

### 2026-09-08 (late) — fs8 + fs8b: **gameplay REACHED with ZERO kick injections — the round-boundary gate alone fixes --fs6**
After the round-boundary gate landed, two consecutive `--fs6` runs (lockstep
driver, harness kick OFF, inline driver kick ON but gated) both reached real
gameplay:

- **fs8** (13:15): `game=2:9` stable from the first watch sample (t=2s) through
  t=90s; gameplay probe passed.
- **fs8b** (13:17-13:21, reproducibility): identical. Watch started with both
  games ALREADY at `game=2:9`; lstat latched `05,01,15,43` (state 5 + 0x40
  success bit), payload `0x50` (player-confirmed), rounds exchanging real
  table data (send/recv cycling 4→3→11→10 etc. = live in-game link traffic).
  Gameplay probe drove both players 20s: frames 13433/13432 advanced, both
  screens live. Screenshot at /tmp/fs_gameplay_final_p1.ppm shows REAL
  co-op gameplay: red Link + green Link standing on a floor-switch platform in
  a lava-cave room with hearts, rupee counter, and a chest.

**Critical detail: the inline kick logged "FS kick: invoking" 112× but
"FS assist: kicked" (actual injection) ZERO times in both runs.** Every
invocation was gated out by the round-boundary check — the games were never
mid-round-stuck in a fireable state, OR they were progressing so cleanly that
the kick never had a boundary+incomplete+phase-machine moment to act on.
Also `assistOn=0 armed=0` the whole run (fs6 never arms the echo), and zero
"did not receive" stalls.

**Interpretation — the kick is not the fix, it was the bug.** fs12/fs7 failed
precisely because the UNGATED inline kick injected mid-round (recv=4-9) and
desynced the games' own round accounting. fs7b (harness kick only) and
fs8/fs8b (gated inline kick, never fired) both succeed because nothing
disturbs the games mid-round. The lockstep driver's OWN transfer pacing —
with the AckPlayer sleep removed (2026-09-07 change: the secondary is NOT put
to sleep when it acks a transfer, so both games observe completions at the
same cycle) — completes the FS post-link handshake on its own. The kick
machinery is at best a harmless safety net (when boundary-gated) and at worst
(ungated) the direct cause of the fs9/fs10/fs12 deadlocks.

**Remaining question for the final patch:** is ANY of the FS-assist kick code
needed? fs8/fs8b say no for the 2P post-link flow (0 injections, gameplay
reached). The echo + unstick paths never ran either (armed=0, 0 stalls). The
candidates for a minimal patch are:
1. lockstep.c AckPlayer sleep removal (+ the HARD_SYNC/ATTACH re-added
   sleeps) — LOAD-BEARING per the fs8/fs8b evidence;
2. the mode-9 / lstat-engaged / round-boundary gates (inert but harmless);
3. the whole crafted-block kick, echo, and unstick stack — believed
   unnecessary for 2P; untested for 4P.

Next: (a) 3rd fs6 confirmation run (fs8c) for reproducibility; (b) decide
minimal patch = strip assist vs keep as host-armed safety net; (c) revert
TEMP traces in io.c/sio.c; (d) verify 4P if feasible.

### 2026-09-08 (late 2) — fs8c: 3rd success, and the gated kick fired on a REAL stall
fs8c (`--fs6`, 13:34-13:39): **third consecutive gameplay reach**, but this
run took the HARD path the earlier two skipped:
- Watch started at `game=9:2` (not already 2:9): both games cycled phase 2↔1,
  rounds timing out (phase=1 round=4) from t=2 through t≈60 — the exact old
  fs9/fs10/fs12 failure signature, live in one run.
- The round-boundary gate fired **2 actual injections** (of 113 invocations),
  both logged as `P1 phase=1 recv=0 got12=1, P2 phase=1 recv=0 got12=1` — i.e.
  ONLY at the genuine deadlock boundary (round timed out, recv parked at 0),
  never mid-round. Context around kick #2 shows FEFE probe traffic still
  flowing (`fincore data=FEFE 0036 FFFF FFFF`) — the games were cycling
  discovery probes while stuck.
- After the kicks the games progressed 9:2 → 5:4 (char select, t=66) → 2:8 →
  **2:9 gameplay, stable**; probe passed (frames 13503/13503).

Interpretation: the boundary gate works AS DESIGNED — inert while games are
mid-round or progressing, but it will fire to break a genuine round-boundary
deadlock. Whether the 2 kicks CAUSED the fs8c breakthrough (vs A-taps or the
driver eventually completing a round unaided) is not proven by this run alone;
the games' own recovery latency varies (fs8/fs8b broke through pre-watch with
0 kicks, fs8c took ~64s with 2 boundary kicks). **--fs9 (pure driver, inline
kick disabled too) is running to resolve causality: if it breaks through, the
kick is decorative; if it stays stuck at 9:2, the boundary-gated kick is the
deadlock-breaker that makes the driver robust.**

### 2026-09-08 (late 3) — fs9 (PURE driver, inline kick disabled): **FAILED — the gated kick is REQUIRED**
fs9 (`--fs9`, 13:43-13:49) = fs6 flow but with the driver-side inline kick
disabled entirely (new `--fs9` mode → `GBASIOLockstepCoordinatorSetFSKickEnabled(false)`;
harness kick already off for lockstep modes). Zero invocations, zero injections.

Result: **GAMEPLAY NOT REACHED (mode=9).** The games stayed at `game=9:2` for
the entire 90s watch — only phase state seen: `phase=1 role=0 a=0 b=0
ready=1 got12=0` (round timed out, cycling). Final screens: `P1=name P2=name`.

**Pair with fs8c → causality resolved:**
| run | inline kick | actual injections | outcome |
|---|---|---|---|
| fs8  | enabled (gated) | 0   | gameplay (natural breakthrough pre-watch) |
| fs8b | enabled (gated) | 0   | gameplay (natural breakthrough pre-watch) |
| fs8c | enabled (gated) | 2   | 9:2 stall ~60s → 2 boundary kicks → gameplay at t=66 |
| fs9  | **disabled**    | —   | **stuck at 9:2 all 90s, GAMEPLAY NOT REACHED** |

**Conclusion: the AckPlayer sleep removal alone is necessary but NOT
sufficient — natural breakthrough is run-to-run luck (fs8/fs8b got it,
fs9 never did). The round-boundary-gated kick is the reliability net**: it
stays inert while the games progress or are mid-round, and fires only at a
genuine round-boundary deadlock (phase=1, recv==0 or >=12, got12 unset),
after which the games advance (fs8c: 2 kicks → 5:4 char select → 2:9
gameplay). The FINAL FIX = AckPlayer sleep removal + the boundary-gated
kick, both already in lockstep.c.

Cleanup done this session: TEMP traces in `src/gba/io.c` + `src/gba/sio.c`
reverted to HEAD (they were pure WARN logging; io.c's SIOMLT_SEND routing was
verified identical to stock). libmgba + harness rebuilt; confirmation run
fs10 (fs6 on the cleaned tree) in progress.

### 2026-09-08 (late 4) — fs10 (cleaned tree) CONFIRMS the fix
After reverting all TEMP io.c/sio.c traces and rebuilding libmgba + harness,
fs10 (`--fs6` on the cleaned tree, 13:51-13:57): **GAMEPLAY REACHED, probe
passed** (frames 13507/13506). Mode progression 9:2 → 5:4 (char select) →
2:8 → 2:9. One actual kick injection at `phase=2 recv=12 got12=1` (round
completed but not rotated to the 13/done state = parked boundary), after
which the games advanced.

**Final fix state (4/4 gameplay reaches: fs8, fs8b, fs8c, fs10; the one
no-kick control fs9 failed):**
1. `lockstep.c` `GBASIOLockstepCoordinatorAckPlayer`: removed the secondary
   sleep on ack (completions now land on the same cycle for both games;
   callers that need the sleep — hard sync, attach — call it themselves).
2. `lockstep.c` `_fsAssistKick` re-gated to round boundaries only: fires
   only when both games are in the FS phase machine (mode 9, lstat engaged,
   valid EWRAM sig) AND parked at a round boundary (recv==0 / recv>=12 /
   recv==-1) AND not all-complete. Never mid-round. Self-gated to the FS
   cart via lazy title check.
3. TEMP traces in io.c/sio.c reverted. Working tree now = lockstep.c/h fix +
   harness dev tools (threaded_link.c, rendezvous.c/h, build.sh) + the three
   doc files.

### 2026-09-09 — Real-app verification session (part 1): app stability + nav fixes + state-set support

Goal this session: verify the lockstep fix in the REAL mgba-splitscreen app (mgba-splitscreen-web
Rust server on :8080), not just the headless harness.

**1. The app's "silent crashes" were a red herring.** Repeatedly launched
mgba-splitscreen-web via the shell and it died ~30s in with no log. Root cause: the
launcher shell's process-group teardown was killing the setsid-less
backgrounded process when the launching command returned/timed out. Launched
with `setsid nohup ... < /dev/null &` it runs indefinitely (2h+ stable at
60fps, 60.0/60.0 fps both players, no OOM/segfault in dmesg). The earlier
"sustained-play segfault" suspicion (to-do.md 🟠) was NOT reproduced; the
mgba-splitscreen-web binary at Sep 8 13:50 + the Sep 9 08:59 rebuild both run clean.
Note: to-do's "tokio-worker segfault at 4a8" may still exist under load but
was not seen here.

**2. raw_ws.py had a real bug that broke nav_fs.py against the web server.**
The server interleaves tagged binary messages: tag 0 = video (307201 B for 2P
RGBA), tag 1 = audio (~4 KB). The Client reader stored EVERY binary message
as `latest`; `player_frame()` then rejected the audio chunks (too small) and
returned None spuriously — nav crashed with TypeError in cursor_pos. FIX:
reader keeps only tag-0 video frames. Also added: `_dead` flag + auto-reconnect
in frame()/send(), and a None guard in nav_fs.reset_cursor_to_a. Added
`cap_frames.py` (robust capture helper).

**3. nav_fs.py detect() misclassifications on the server rendering.**
- Name-entry keyboard classifies as "saving" (purple dialog + dark bg):
  reordered the name check (purple_mid>0.20 AND light_rows>=8 AND light_total
  >100) BEFORE the generic saving check.
- File-select ALSO satisfies the purple+keyboard test (same menu palette;
  red/pink slot banners ≈ keyboard density: purple 0.588/0.586, rows 26/26,
  total 250/248 — nearly identical metrics on name vs file select!). The
  bottom button band discriminates well: name = 11 light clusters (4 buttons),
  file select = 2 clusters (COPY/ERASE) — not yet wired in.
- The HARNESS (threaded_link.c) tolerates all of this: it never depends on
  the classifier; it WARNs "not at name entry" and CONTINUES the fixed tap
  flow, which converges because the A-taps push the menus forward regardless.
  Ported that resilience into nav_fs.py: to_name_entry no longer aborts the
  player flow when the name screen isn't positively detected.
- Measured input/transition latency on the web server: ~0.4s per menu
  transition (NOT the 2s+ feared earlier) — the nav cadence is fine.

**4. Nav reached the FS select on the server** (both players typed names,
saved, selected Four Swords) but the classifier's wrong labels made the
post-save waits time out; screens ended at name-entry (P1, "AB" typed) and
file-select (P2, file "AAA" saved). Session ended there; the remaining nav
work is the button-band discriminator + re-run — BUT the user offered a 4P
save state at the pre-link screen, which bypasses menu nav entirely.

**5. Save-state set export/import added (all three frontends).**
The app only had IN-MEMORY quick states (DUALSTATE blob: "DUALSTATE"|ver u32
LE 1|count u32 LE|(size u32 LE, bytes)*). Added:
- web_server.rs: GET/POST `/state` (download/upload the full state set;
  POST calls EmulationManager::load_state_set which resets lockstep drivers
  after restore so no player is left sleeping).
- lib.rs (Tauri): `export_state_set(path)` / `import_state_set(path)`.
- index.html + main.js: "Export State Set…" / "Import State Set…" buttons
  (wasm mode builds/parses DUALSTATE from wasmStates; socket mode fetches
  /state; Tauri invokes the new commands). Tested: GET /state returns a
  794649-byte DUALSTATE blob with the ROM loaded; POST round-trip = ok.
- The browser WASM build (mgba-splitscreen/web/*) was STALE (Aug 25) — rebuilt Sep 9
  08:56 with the lockstep fix; clean build, no warnings. GitHub Pages ships
  these tracked files on deploy, so committing is what pushes the fix to
  production web.

NEXT (this session): get the user's 4P pre-link save state loaded (via the
new Import State Set) and watch the server log for the link handshake →
char select → gameplay on the REAL app.

### 2026-09-09 — Solo keyboard mode (testing helper) for the web/desktop UI
Request: let one tester drive all players with ONE control scheme by toggling
which player the keyboard controls with the 1-4 digit keys.

Implementation (mgba-splitscreen/src/main.js, index.html, styles.css):
- `soloKeyboard` mode (default ON, persisted `mgba-splitscreen_solo_v1`): digits 1-4
  switch the ACTIVE player; P1's control map drives only that player via
  `keyStates[soloPlayer]`/`sendKeys(soloPlayer)`. "Solo Keyboard: On/Off"
  toggle added to the Players menu + an "Active: Px" badge in the menubar.
- Active player re-clamped on player-count changes; held keys released on
  player switch / mode toggle so buttons never stick.
- BUG found + fixed while testing in the live preview: the Players-menu wiring
  (`document.querySelectorAll('#player-menu button')`) attached the player-count
  click handler to EVERY button in that menu — including the new toggle, which
  has no data-players → clicking it ran `setPlayerCount(NaN)` (NaN passes the
  n<1||n>4 guard) and corrupted `playerCount` ("Active: PNaN", "digits 1-NaN").
  Fixed three ways: `Number.isInteger` guard in setPlayerCount, and both
  #player-menu iterators skip buttons without data-players.
- Verified live in the preview (http://127.0.0.1:8080, in-browser WASM engine):
  digit switching P1→P2→P3→P4→P1, toggle OFF/ON clean at 2P and 4P, no NaN.
  Key-routing branch mirrors the existing per-player loop (single index).

### 2026-09-09 (round 4) — Browser P1 divergence root-caused: WASM import never resets the link driver
User round-3 report: with the patched state imported, P1 froze at the linking
screen (rest fine) — console showed kick firing (`enabled=1`, cart identified)
but ZERO injections, plus hundreds of `Out of bounds ROM Load32: 0x0D000000`
on P1. Server-side run of the SAME file: 0 OOB reads, kick injected 219x.

Root cause found by diffing the two state-load paths:
- `emulation.rs load_state_set` (Tauri/server): after restoring all cores it
  calls `d.reset` on every driver ("abort any stale transfer and wake the
  players so the games re-handshake"). This is what makes the server path
  work, together with the 09-09 GBASIOLockstepDriverReset queue-clear.
- `mgba_splitscreen_web.c mgs_load_state_bytes` (browser WASM): just `core->loadState`,
  NO driver reset. The restored mid-link coordinator queues/asleep flags
  stayed live, P1's first handshake rounds diverged, and its game ended up in
  a different code path that polls 0x0D000000 forever.
- 0x0D000000 is a REAL game constant (59 LE hits in the ROM) — reads there are
  the game polling, not "executing garbage" per se; it's the SYMPTOM of P1
  taking a divergent path, not the cause.

Fixes this round (all rebuilt + server-restarted 12:17, pid 119307):
1. `mgs_reset_sio()` exported from mgba_splitscreen_web.c — calls `g_drivers[i].d.reset`
   after ALL players' states are loaded (must use the vtable: the static
   GBASIOLockstepDriverReset isn't in the header). Wired into BOTH wasm import
   paths in main.js (importStateBrowser + quickLoadState).
2. Kick decline diagnostics in lockstep.c: `_fsKickLogDecline()` logs WHICH
   self-gate failed + every player's (mode, lstat, ssub, role, ph, recv,
   got12) — rate-limited (once per distinct reason + every 32nd decline).
   Verified live: early run shows `FS kick declined (not-in-link-screen):
   P1(mode=0 lstat=0 ssub=2 role=8 ph=1 recv=13 got12=0) ...` then injections
   once mode=9. No more blind "invoking forever".
3. Session-log forwarding bug: `sessionLogFlush` only re-armed its timer when
   there was new data, so the first no-op flush killed the chain and the whole
   session sat in the buffer until unload — where fetch gets canceled. Fixed:
   re-arm unconditionally + `navigator.sendBeacon` on beforeunload.
4. Server MIME: `.wasm` fell through to `application/octet-stream` → browser
   refused streaming compile every load ("Incorrect response MIME type",
   fallback worked but noisy). Added `Some("wasm") => "application/wasm"`.

Round-4 server verification (app_run11.log): ROM+state 200, bootleg=0,
oob_rom=0, bad_memory=0, bad_bios=0, dma_invalid=0; all 4 players steady
60.0 fps; `FS kick: invoking` x23, `kicked deadlock` x2, declines explain the
early gate misses. session_log endpoint 200. WASM + native + harness rebuilt;
harness relinked against fresh libmgba.a.

NEXT: user browser retest — hard reload (new WASM+MIME+session log), 4P, load
ROM, Import State Set (patched mgba-splitscreen.dualbystate), START on all 4. Expected:
no streaming-compile error, no bootleg/OOB/bad-memory spam, `FS kick declined
(...)` lines showing WHY if the kick still can't inject, and — if the reset
fix holds — `kicked deadlock` + games advancing past the linking screen. If
P1 still diverges, the decline line will show P1's mode/lstat values vs the
others and we'll know exactly which gate blocks.

### 2026-09-09 (round 5) — Browser kick INJECTED; 4P slot fill fix
User round-4 report: no freezing, kick machinery firing, and the FIRST browser
injection: `FS assist: kicked deadlock (P1 phase=1 recv=0 got12=1, P2 phase=1
recv=0 got12=1)`. P1 "gets to the right state" but P2-4 never do.

Root cause (in _fsAssistKick): the crafted deadlock block was written into
slots 0-1 of the +40/+44 recv histories with slots 2-3 hardcoded FFFF — the
2-player assumption ("no 3rd/4th device"). Harness comment confirms the game
mechanics: slot k receives player k's value each round and the acceptance
scan (0x800C6A8) accepts each slot whose sum == -15, only present players
count. With 4 players the games' natural tables have all 4 slots valid, so
FFFF in slots 2-3 made P2-P4's scans reject every round; P1 (master, also the
only one given recvIdx=13) advanced alone.

Fix: fill the crafted block into slots 0..nAttached-1 (FFFF for the rest) —
behaviorally IDENTICAL for 2P, correct for 4P. Also made the injection log
dynamic (prints all attached players, not just P1/P2).

Server-side verify (app_run12.log, new binary): ROM+state 200, 0 OOB, kick
invoking x26, and `kicked deadlock (P1 phase=2 recv=13 got12=1, P2 phase=2
recv=13 got12=1, P3 phase=2 recv=13 got12=1, P4 phase=2 recv=13 got12=1)` —
all four reach the phase-2/recv-13/got12-1 validated state, same signature as
the 2P-proven gameplay point.

NEXT: user browser retest (hard reload, import patched state, START x4). If
P2-4 now advance with P1 → 4P gameplay. Watch for `kicked deadlock (P1...
P2... P3... P4...)` with all four at recv=13.

### 2026-09-10 (round 6) — ROOT CAUSE: the FS link-screen assist was never armed

User round-6 report: "P1 gets to the right state, P2-4 never do" (console:
`FS kick declined (mid-round-progress): P1(mode=9 lstat=4 ssub=4 role=8 ph=2
recv=2 got12=0) P2-4(mode=9 lstat=0 ssub=3 role=0 ph=2 recv=2)`, MULTI
"did not receive data" flooding). The forwarded `/tmp/mgba-splitscreen_session.log`
showed a different, later run: 192 OOB reads at import, `not-in-link-screen`
declines at mode=0, a BIOS reset-vector burst and `0xffffe0xx` bad-memory
spins — i.e. the games restarting after the import.

**ROOT CAUSE (found by grepping who arms the assist):**
`GBASIOLockstepCoordinatorSetFSArmed()` — the switch that turns on
`_fsAssistTick`'s discovery echo — is called by exactly ONE caller in the whole
tree: the test harness (`threaded_link.c:735/926`). Neither `emulation.rs`
(desktop + web server) nor the WASM bridge (`mgba_splitscreen_web.c`) nor `main.js`
ever arms it, so `fsAssistArmed` stayed false forever and `_fsAssistTick`
returned at its `if (!fsAssistArmed)` guard. The mechanism the harness's
proven chain depends on ("the games exchange FEFE probes whose counters stay
offset … this assist delivers each recipient ITS OWN sent value in every
slot") has been dead code in the app and the browser this entire time. That is
why every front-end session stalls on the FS linking screen while the harness
walks to name entry: the harness is the only run that ever armed it.

**Fix (src/gba/sio/lockstep.c):** self-arm in `_fsAssistKick` from the same
signal the host was supposed to provide — the FS cart is identified AND every
attached player is sitting in link-screen mode (IWRAM 0x6D10 == 9, which the
title/menus are not: they sit at 0/8/13). Logs
`FS assist: armed at the link screen (all N players in mode 9)`. Host calls
remain possible (the harness's explicit `SetFSArmed(true)` is a no-op now, and
the echo still hands off to raw data after real payload rounds).

**New tooling: `mgba-splitscreen/src-tauri/tests/fs_link_repro.rs`** (ignored by
default). Reproduces a browser session natively against the SAME cooperative
model the WASM uses, without a WASM rebuild: `EmulationManager::new(N)` →
`load_rom` → `load_state_set` → START → A+B confirm → dump each player's
screen to `/tmp/fs_repro_*.ppm`. Env overrides: `FS_LINK_PLAYERS`,
`FS_LINK_STATE`, `FS_LINK_TAG`, `FS_LINK_CONFIRM_MS`. Run with
`cargo test --release --test fs_link_repro -- --ignored --nocapture`.

**Finding — the captured state set is a dead end.** The user's
`mgba-splitscreen.dualbystate` was captured MID-HANDSHAKE (blob: mode=9, lstat=0,
ssub=3 for all 4; P1 role=8 phase=2 recv=13). After restore NO transfers ever
resume: the native repro, the threaded harness (`--fs11`, which logs
`Transfer starting` = 0, `MULTI transfer finished` = 0) and the browser all
end with the games parked on "Linking with other systems… Please wait a
moment." and `mlt=FFFF,FFFF,FFFF,FFFF`, because the core state carries the SIO
registers but the coordinator's pending transfer/ack events are not restored
(the driver reset added in round 4 deliberately aborts them). Screenshots
(`/tmp/fs_repro_*_p*.ppm`) confirm the same screen for 2P and 4P alike, so the
stall is not 4P-specific — the imported state is the confound. The proven path
is a FRESH BOOT + navigation to the linking screen.

**Stall signature at the live link screen** (what the kick sees): mode=9,
lstat=0, ssub=4, role=0, phase=0/1, recv=13, got12=0 → `_fsAssistKick`
declines `phase-sig-mismatch` (its gate wants phase 1/2, the post-name
signature). phase=0 is the games' post-retry reset (the FS link machine gives
up after `round=4` and clears the role/phase bytes), so the next kick work
item is recognising that reset signature.

**Shipped this round:** WASM rebuilt 11:17 with the self-arm
(`mgba-splitscreen/web/mgba-splitscreen-web.wasm` = 782,136 B, copied into `mgba-splitscreen/src/`),
server restarted (pid 542247) and verified serving it as `application/wasm`.
The desktop/web-server binary was rebuilt too, so the app carries the fix.

NEXT: (1) user test from a FRESH boot (load ROM, navigate to the Four Swords
linking screen) rather than the state import — the assist should now complete
the discovery handshake; (2) teach the kick the post-retry reset signature
(phase=0, recv=13, ssub=4) so a genuine link-screen stall still gets kicked;
(3) if 4P still diverges from a fresh boot, compare per-player MULTI delivery
in the 4P path (`_setData`/`AckPlayer` ordering) against the 2P-proven run.

## Round 7 (2026-09-10 evening): the kick was silently refusing; sub-state
machine fully instrumented; ssub-4 A-confirm is the last gate

Four concrete fixes shipped this round, each verified live in the browser or
native repro (state-import flow, `mgba-splitscreen.dualbystate`):

1. **allComplete-frozen gate** — `_fsAssistKick` returned SILENTLY when every
   player read recv==13 && got12==1 (the exact signature the state file
   restores into and the games freeze at), so it never injected, never
   declined, forever. Now only "complete" counts as healthy while recv is
   actually MOVING between kick attempts; frozen-at-13 gets kicked like any
   11/12 stall. First kick fired immediately after this fix.
2. **Echo immediate-engage disabled** — engaging the echo as soon as armed
   made every game receive the MASTER's value in every slot, so P2-P4 never
   saw their own checksums echoed back and the per-slot checksum validation
   failed (the games detect all four units — badges draw — but the checksum
   set never validates). Raw-data discovery restored; the echo only re-engages
   via the 120-quiet-round handoff.
3. **Link-status success latch** — the games' own rounds overwrite the
   crafted +44 histories before the state-5 payload check runs, so
   `[0x03000FC3]|=0x40` never set and ssub-4's A-confirm stayed closed. The
   kick now sets `iw[0x0FC3]|=0x40` and `iw[0x0FC8]=2` directly. Note: the
   game CLEARS 0x0FC3 back to 0 on every discovery restart, so the latch is
   only meaningful while the machine holds.
4. **Phase re-entry** — P2-P4's machines halted at phase=0 (post-retry
   reset) with the crafted block + got12 present but the acceptance scan
   never running; the kick now lifts `st[1]` 0->1 when injecting. After this:
   ALL FOUR read `phase=1 recv=13 got12=1 ssub=4 fC3=40 fC8=2`.
5. **Kick log now prints ssub / lstat / fC3 / fC8 per player** — the run
   above shows P1's sub-state machine ADVANCING under A-taps (ssub 1->2->3->4,
   phase=2) while P2-P4 sat at ssub=4 phase=0, the asymmetry that pointed at
   phase re-entry.

**Also learned:** A+B on the linking screen is a CANCEL (observed in
cooperative-repro screenshots: games back at the FS title 6s later) — the
confirm is A-taps at the right sub-states (harness: ssub=3 needs A to start
linking, ssub=4 needs A again after >120 frames of active link). The user's
START-only sessions never pressed A at all.

**Where it stands:** rounds flow cleanly (ack barrier), assist arms, kick
injects with full visibility, and all four machines now HOLD at `phase=1
ssub=4 fC3=40 fC8=2` — but ssub-4's A-confirm still does not fire the games
forward; the screens stay on "Linking with other systems…" in every model
(cooperative repro, threaded harness, browser).

NEXT: static analysis of the ROM's ssub-4 A-confirm handler (the link-status
machine at 0x03000FC0, state-5 payload check 0x8037b5c, the >120-frame
counter the A-check is gated on) to find what else must be set before the A
press advances the machine — the last gate between here and name entry.

## Round 8 (2026-09-11): the link layer is VERIFIED GOOD at 4P — stop
blaming SIO; plus a real perf bug in the assist

### 1. Ground truth: 4-player MULTI works (new test `tests/linktest_4p.rs`)

Pointed the existing linktest instrument ROM (`mgba-splitscreen/linktest/main.c`, which
renders live MULTI diagnostics) at `EmulationManager` — the **same cooperative
stepping model the browser WASM uses** (one thread, sleeping primary skipped,
same `mgs_run_frame` path). At 4 players, 20 s:

```
P1 MASTER  SIOCNT 2005  S0 7F01 S1 7E05 S2 7E05 S3 7E05
           TX 893  RX 892   STALL 0   LINK ACTIVE - 4 PS   PEERS 3
           RTT BEST 1 WORST 2
P2 SLAVE   ME ECHO  PEER SEND  EXP WAIT  GAP 1  STALL 0
           LINK ACTIVE - TRANSFERS FLOWING
```

* TX and RX climb in lockstep (1251/1250 by 21 s), STALL stays 0, all four
  units enumerate each other (4 PS, PEERS 3), RTT ~1 frame.
* **All four units agree on the SIOMULTI0-3 contents** — exactly GBATEK's
  stated invariant ("after the transfer, all connected GBAs will contain the
  same values in their SIOMULTI0-3 registers"), with slot k = unit k's sent
  value. So the slot mapping in `FinishMultiplayer` is spec-correct; the old
  "maybe the mapping is wrong" hypothesis is dead.
* All four instances hold 60 fps (`frames=[1262,1262,1262,1262]`).

**Conclusion: the lockstep SIO layer is not the bug.** Four-player MULTI
exchange is clean and correct in the exact browser stepping model. Whatever
parks Four Swords on the linking screen is above the link layer.

### 2. A/B on the FS flow: the assist changes nothing

`fs_link_repro` at 4P, with the new `MGBA_SPLITSCREEN_FS_ASSIST=0` kill switch (added
this round) versus the assist/kick active — same state-import flow, ~73 s:

| | stalled transfers | kicks | final screen |
|---|---|---|---|
| assist ON  | 0 | 37 invoked / 4 injections | "Linking with other systems…" |
| assist OFF | 0 | 0 | "Linking with other systems…" |

Both runs are ALIVE (frame-diff shows the link screen's blink animation), both
sit on the same screen, and neither stalls a transfer. The ack barrier (Round 6)
really did fix the historical `MULTI did not receive data` flood — with it in
place the unassisted path is already clean, so the echo/kick machinery buys
nothing. Read this as: the assist is not the fix, and it is large uncommitted
surface area that has never made FS advance.

### 3. Real bug fixed: the assist flooded the log on EVERY other game

`_fsAssistIdentify` re-probed the cart title (and logged the failure) from every
transfer completion and every kick attempt. On a non-FS ROM the linktest run
produced **25,000+ `FS assist: identify failed` lines in 20 seconds**, plus a
`FS kick: invoking` line twice a second forever. Fixed: probe once and cache
(`fsAssistChecked`), demote/log once, and move the kick's "invoking" log past
the FS-cart gate. After: exactly one line (`FS assist: not the Four Swords cart;
assist off for this session`). This was a real cost for every user of every
other game, not just a diagnostic annoyance.

### 4. New tooling

* `mgba-splitscreen/src-tauri/tests/linktest_4p.rs` — runs the linktest instrument ROM
  through `EmulationManager` at 2-4 players and dumps `/tmp/linktest_*_p*.ppm`.
  This is now the fastest way to prove/disprove any SIO regression.
* `MGBA_SPLITSCREEN_FS_ASSIST=0` — loads FS with the assist and the deadlock kick fully
  off, so FS's native behaviour can be compared against the assisted path
  without a rebuild. Also the recommended setting while debugging: the assist
  has never advanced the game and only muddies the trace.

## Round 9 (2026-09-11): two red herrings killed, the assist disabled by
default, and a console/log-capture fix

### 1. The `Bad memory` / `Out of bounds ROM` floods are NOT a crash

The user's browser logs (and every browser session here) are dominated by
`GBA Memory: Bad memory Load8: 0xffffe0XX` and `Out of bounds ROM Load32:
0x0D000000`, which looked exactly like "the game is executing garbage because the
assist corrupted it". Tested directly: **one player, no link cable, no assist**,
the good ROM, in the browser -> same flood AND the game renders its CHOOSE A FILE
screen perfectly and plays. So the reads are the game's own routine probing of
unmapped space, they are benign, and they have been mislead for several rounds.
(Also: the native builds never print them, so comparing native vs browser logs on
this signal was apples-to-oranges. The WASM bridge raises the log level.)
ROM identity checked to be safe: `mgba-splitscreen/src/fs_rom.gba`,
`mgba-splitscreen/src/tmp-test/fs.gba` and `Test Roms/…Four Swords (U) [!].gba` are all
md5 `3287ca66e5cc285a9fe3a922051e84c6`, title `GBAZELDA`, 8 MiB — a good dump.

### 2. 2P fails exactly like 4P

The user's 2P log shows the same discovery cycle as the 4P run (`hs=1` on the
same round shape, echo never engaging, `FS kick` firing) — so this was never a
4P-specific bug. Worth remembering before bisecting player counts.

### 3. The assist is now OFF by default (opt-in `?fsassist=1`)

New `GBASIOLockstepCoordinatorSetFSSuppressed()`, exported to JS as
`mgs_set_fs_assist(int)` in `mgba-splitscreen/web/mgba_splitscreen_web.c`, called from `main.js`
after every `_db_init()` (the coordinator is recreated there, so the setting is
re-applied on each player-count change/reload). Rationale in the C comment: the
assist pokes the game's private link state every ~2 s and has never moved FS off
the linking screen in any model, so it should not be on by default. A/B:
`http://127.0.0.1:8091/` (off) vs `http://127.0.0.1:8091/?fsassist=1` (on).

**Verified live** (linktest ROM, clean console):
`GBA Serial I/O: FS assist: suppressed by host`.

### 4. Fixed: `/session_log` retried 501s forever

`sessionLogFlush()` re-POSTed every 2 s regardless of the response, so on the
documented static-file preview route (no POST handler; Python answers 501) the
console filled with hundreds of identical errors and buried the actual game log.
It now gives up after the first refusal and says so once:
`session log endpoint unavailable (501); console capture disabled (emulation is
unaffected)` — verified live, and confirmed by killing the stale page and
watching server-side `POST /session_log` drop to zero.

### 5. Preview served on 8091, not the runbook's 8090

`main.js` is loaded as `<script type="module" src="main.js">`, so busting the
*page* URL with a query string does **not** invalidate the script — the browser
served the old module from cache (`decodedBodySize` 94932 vs the new 97313) and
every "verify the fix" run was silently testing old code. Serving from a
**different port** (a different origin, hence a different cache) is the reliable
escape. Keep 8091 for this thread; the runbook still says 8090 and should be
updated (it is currently not writable by the agent).

NEXT: get a **fresh-boot** baseline (the state-import flow restores into a
mid-handshake freeze and cannot reach gameplay, so every A/B above is run on a
confound). Drive FS from boot through file-select → name entry → CHOOSE A GAME →
Four Swords at 2P and 4P with `MGBA_SPLITSCREEN_FS_ASSIST=0`, and compare: if 2P reaches
name entry and 4P does not, the bug is in FS's 4-unit expectations, not SIO.
`mgba-splitscreen/scripts/link_test.py` already drives that menu path over the websocket
for the web build.

### 6. Found the regression: the "H1 experiment" was never undone (2026-09-11)

Bisected with git instead of more black-box pokes. Comparing the last documented
working state to today:

```
git diff e191ddf9b..HEAD -- src/gba/sio/lockstep.c src/gba/io.c
```

is only three functional lines in `lockstep.c`:

1. `UNLOCKED_INTERVAL` 4096 -> 8192,
2. the `nextHardSync` restructure,
3. **`_hardSync(coordinator, player);` commented out** at the end of
   `GBASIOLockstepDriverFinishMultiplayer`.

(1) and (2) are upstream's `a0647ffac` "Loosen timing where possible" — the patch
the branch deliberately cherry-picked, and the patch that `PROJECT_LOG.md`'s
2026-08-17 entry credits for FS 2P finally passing the linking screen into
character-select. (3) is the *H1 experiment* from `a9ca9b441`, whose recorded
outcome is "the handshake still cycles" — so it fixed nothing and only removed the
per-round realignment. It was left in place. Restored the call.

Also corrected a long-standing measurement error: `mgba-splitscreen/tools/threaded_link`'s
"A+B confirm" presses **B**, which on the FS link screen *cancels* the link, and
its `dump_ppm(g_cur[...])` reads a lagging snapshot buffer. In this run
`fs_ab_before` is the intro cutscene and `fs_postlink` is the linking screen, and
the frames it then watches are the title-screen attract demo. Every harness
pass/fail that rested on `cur_screen()` or those PPMs is void; judge on the live
browser screen.

Built and served on a **fresh origin**: `http://127.0.0.1:8092/` (static server
over `mgba-splitscreen/src`, pid in `.freebuff/preview-*.log`), WASM 784707 B and verified
byte-identical to the fresh `mgba-splitscreen/web/build.sh` output, with `strings`
confirming the old H1 comment is gone from the served module.

### 7. Verified: Four Swords 2P now links and plays in the browser (2026-09-11)

Driven live in the preview against the freshly built WASM:

- load the ROM, 2 players → `CHOOSE A FILE`;
- `File → Import State Set…` with `fs_state.dualbystate` (captured at the FS
  title) → both games at "PRESS START";
- START on both, near-simultaneously → both on
  "Linking with other systems… Please wait a moment.";
- tap A on both → both on **"CHOOSE A STAGE" / Chambers of Insight**;
- tap A again → both in the **dungeon**, green P1 Link and red P2 Link on screen
  in the same room, hearts + rupee HUD, both views in sync.

Console for the whole run: `nAtt=2`, live `SIOMLT_SEND` values exchanging
(`0150`, `FCE5`, `02C4`, `004A`…), **zero** `MULTI did not receive data`, no kick
lines (assist still off by default). So the H1 removal alone was what pinned FS on
the linking screen forever. 4P still needs a clean run with all four games at the
link screen together (the imported 4P state set is inconsistent across players).

---

## 2026-09-11 — Renamed to mgba-splitscreen; Desktop (Tauri) verified; v0.3.0 tagged

### The fix landed, and it is not Four-Swords-specific

Committed as `3a7060bb8` (the `_hardSync` restore) and `2db68d877` (assist off by
default in the desktop app). The user confirms it works **across multiple link
games, not just Four Swords**, which also retires the "maybe FS expects
something special" line of investigation.

### Desktop app: chosen default was the last blocker

The desktop path never called `GBASIOLockstepCoordinatorSetFSSuppressed`, so the
native app armed the assist *and* its IWRAM-poking deadlock kick while the browser
suppressed both. `EmulationManager::new` now suppresses it right after
`GBASIOLockstepCoordinatorInit`, matching the browser (`?fsassist=1` remains the
A/B opt-in). Verified live at startup: `[mGBA] FS assist: suppressed by host`.

### What the desktop session actually showed (method note)

Verifying the wrapper needed a *visible* screen: this session drove the app over
its WebSocket (`scripts/raw_ws.py`) and rendered the captured frames as a
data-URI gallery in the preview, because `nav_fs.py`'s screen classifier is
unreliable — it reported "file"/"name" for frames that were demonstrably the
CHOOSE A GAME / linking screens. Two traps worth remembering:

- **B cancels the link** and returns to the title; A+B is not a confirm.
- Frames from the WS arrive **BGR-swapped**; swap R/B before judging colour.

Also uncovered: `quit_game` → `load_rom` over the WS leaves the emulator at
0 fps with no frames at all. Restart the process instead.

### Rename (this commit)

Product, directory, crate/npm/bin names, WASM artifacts, identifiers, docs and CI
are all `mgba-splitscreen` now. The GitHub repo already was, so nothing there
changed. Details worth knowing:

- `DualBoy/` → `mgba-splitscreen/`; `dualboy_web.c` → `mgba_splitscreen_web.c`;
  `dualboy-web.{js,wasm}` → `mgba-splitscreen-web.{js,wasm}` (WASM rebuilt).
- The WASM bridge prefix `db_*` → `mgs_*`, covering `EXPORTED_FUNCTIONS` in
  `web/build.sh` and every `_mgs_*` call in `main.js`; `EXPORT_NAME` became
  `MgbaSplitScreenWasm` (a hyphen would have been an invalid JS identifier).
  Third-party `db_` in `src/third-party/sqlite3` deliberately untouched.
- State-set extension `.dualbystate` → `.mgsstate`, still *accepting* the old one
  on import. The on-disk `DUALSTATE`/`DUALSAVE` magics were left alone on purpose:
  changing a format tag would strand the local state sets the repro tests load.
- Versions unified to **0.3.0** (Cargo said beta.1, npm/tauri said alpha.1).
  Tagged `v0.3.0`, which triggers `release.yml` to build Linux/macOS/Windows
  bundles. macOS and Windows cannot be built on this Linux box.

### 2026-09-11 (release pipeline) — two CI bugs stood between the fix and a release

Getting v0.3.0 published took three tag runs, and both failures were real
defects rather than flakiness. Worth knowing before the next release:

1. **The rename commit carried stale file contents.** `git mv` ran before the
   text pass, so the index held the renames with their *pre-rename* contents
   and the 39 modified files were left unstaged. `git log --stat` looked perfect
   (all renames) while shipping `DualBoy` strings inside. Fixed by staging the
   content properly (commit `9722462b2`); `git grep -i dualboy HEAD` is the
   cheap check that would have caught it before pushing.

2. **Windows never ran `beforeBuildCommand`.** Tauri runs it through a shell on
   Linux/macOS but *directly* on Windows, so `node -e "const fs=require('fs')…"`
   arrived with the wrapping double quotes attached and node died on
   `SyntaxError: Invalid or unexpected token` before executing a line. Any JS
   embedded in `tauri.conf.json` is therefore Unix-only. Replaced with
   `npm run strip-web-engine`, which also removes the working-directory guess
   (the old command walked up from `process.cwd()` hunting for
   `src-tauri/tauri.conf.json`, which is a tell that cwd is not the package
   root; `npm run` finds the package from either place).

3. **The release job published the wrong thing.** `path: bundle/**` shipped the
   unpacked AppImage tree too — ~250 files including its bundled `.so`s,
   `copyright`, and the raw executable — and duplicate names across the
   deb/rpm/AppImage trees make `softprops/action-gh-release` fail with
   `Not Found … update-a-release-asset` before any release exists. This is why
   **v0.2.0-alpha.1 also failed at exactly this step while all three of its
   platform builds passed**: a long-standing bug, not rename fallout. The
   upload now lists the installers explicitly.

Local-only gotcha, for anyone building the desktop app right after a directory
rename: the cached `target/release/build/tauri-*` output embeds absolute paths
from before the move, so `cargo build` dies with
`failed to read plugin permissions: …/DualBoy/src-tauri/…`. Delete
`target/release/build/tauri-*` (keeping the expensive `mgba-splitscreen-*`
cmake output) and rebuild. CI is unaffected — it always starts clean.
