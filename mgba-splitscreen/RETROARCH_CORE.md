# RETROARCH_CORE.md — shipping mgba-splitscreen as a RetroArch core

> **STATUS 2026-09-29: BUILT AND VERIFIED IN RETROARCH.** Phases 1–4 done plus
> the local half of phase 5. `mgba_splitscreen_libretro.so` loads and runs in
> RetroArch 1.20: 1P quick path renders; `--subsystem gba_link_4p` with 4×
> `mgba-splitscreen/linktest/linktest.gba` attaches the link (`link attached:
> 4 players`, wire ids 0-3), composites 480x320, and all four quadrants reach
> the green link-active status line (master shows `LINK ACTIVE - 4 PS`) —
> evidence: `docs/screens/retroarch-4p-linktest.png`. Standalone harness runs
> 2P/3P/4P × 600 frames with serialize roundtrip OK. Save states: mGBA core
> states + lockstep driver state per player, header = SPST magic + version +
> nPlayers + per-slice lengths (`sp_serialize` in instances.c); do NOT reset
> the coordinator on unserialize (driver state rides the savestate already;
> recreating it frees live GBASIOLockstepPlayer objects → crash). The two
> bugs that bit hardest, both worth remembering: (1) the mLockstepUser
> callbacks recover their sp_player by casting `user` back — lockstepUser
> MUST be the first struct member (compile-time asserted); (2)
> `mCoreSaveStateNamed` seeks its target VFile to 0, so serialize each core
> to a scratch VFile and concatenate. Remaining: Mario Kart/FS end-to-end
> checks, libretro-super/docs PRs + GitLab mirror ask (out-of-band, needs a
> maintainer).

> **Implementation-ready spec.** Written 2026-09-28 after verifying the upstream
> libretro port in-tree, the official core-submission pipeline, and the closest
> existing precedent (TGB Dual). Plan/rationale: `to-do.md` (RetroArch section)
> and `history.md` (2026-09-28 entry). Companion spec to `JOIN_CODES.md` —
> same format: goal, design, file-by-file phases, edge cases, verification,
> build order.

## Goal

A RetroArch user loads one ROM with the **mgba-splitscreen core**, RetroArch
asks how many players, and a 2–4-up splitscreen GBA session runs inside the
normal RetroArch shell: RetroPad input per player, fast-forward, save states,
shaders, streaming — everything cores get for free. Eventually: distributed in
the official Core Downloader (buildbot nightly builds).

## The central design question (settled)

**libretro is a single-instance API**: the docs say implementations are
"designed to be single-instance, so global state is allowed," and one core = one
`retro_run()` = one video frame. Multiple linked cores cannot be *separate*
core instances. But the link-cable integration **does not need to be** — and
there is direct precedent: **TGB Dual (tgbdual-libretro)** instantiates two GB
emulators *inside one core*, links them internally, and overlays both screens:

```
g_gb[2]                      // two emulator instances in one core
{ "2 Player Game Boy Link", "gb_link_2p", gb_roms, 2, RETRO_GAME_TYPE_GAMEBOY_LINK_2P }
→ retro_load_game_special(RETRO_GAME_TYPE_GAMEBOY_LINK_2P, info, 2)
```

It also declares **per-instance SRAM/RTC** (`retro_subsystem_memory_info` →
`RETRO_MEMORY_GAMEBOY_1_SRAM` …), runs both GBs' frames per `retro_run()`,
composites both screens into one video frame (layout + which-players core
options), and mixes audio (`tgbdual_audio_output: #1|#2|both`). That is exactly
our architecture at N=2. Our fork's sequential frame loop
(`mgba-splitscreen/src-tauri/src/emulation.rs` — cooperative budgeted stepping
with per-player `sleeping_flags` bridging the lockstep sleep/wake to a
single thread) is the proof this model works; the libretro core re-expresses it
in C against the same C APIs.

So: **one core instance containing 2–4 linked mGBA cores**, driven by a
libretro **subsystem** (`retro_load_game_special`) exactly the way TGB Dual
does it. mGBA's own headers already assume 4 (`MAX_GBAS 4` in
`include/mgba/internal/gba/sio.h`), and mGBA upstream already ships the
*driver* this fork uses (`GBASIOLockstepCoordinator`/`Driver` in
`src/gba/sio/lockstep.c`) — the same code the web and desktop apps drive.

## Requirements to BE a libretro core (the API surface)

Compiled as a shared library exporting the `retro_*` symbols from `libretro.h`
(canonical copy: `libretro/libretro-common/include/libretro.h`; this repo
already vendors a copy at `src/platform/libretro/libretro.h`). Frontend call
order is fixed; global state is allowed. The essentials:

| Callback | Contract | Our job |
|---|---|---|
| `retro_set_environment` / others | Frontend stores its callbacks in the core | Store; nothing fancy |
| `retro_init` / `retro_deinit` | Once, around the whole session | Allocate the N-instance manager |
| `retro_get_system_info` | Name, version, **`need_fullpath=false`** so soft-patching works | Static strings |
| `retro_load_game` | Load content; query player count via `RETRO_ENVIRONMENT_GET_VARIABLE` | Single-player path: 1 instance, no link |
| `retro_load_game_special` | Multi-ROM subsystem entry (2–4 ROM entries) | The splitscreen path: N instances + coordinator |
| `retro_get_system_av_info` | Fixed `timing.fps=59.727500`, audio 32768 Hz; geometry = the composite | W×H changes at runtime → `SET_SYSTEM_AV_INFO` |
| `retro_run` | Poll input ≥1×, run one frame of emulated time, call video **exactly once**, push batched audio | The frame loop (below) |
| `retro_set_controller_port_device` | Frontend assigns RetroPads to ports 0..N-1 | Map port p → player p's keymask (upstream's `keymap[]` order) |
| `retro_serialize`/`unserialize`/`serialize_size` | Size must never increase; per-frame for rewind/netplay | Serialize coordinator + all N cores (mGBA's `mCoreSaveStateNamed`) |
| `retro_get_memory_data/size` | Battery save per instance | TGB Dual pattern: `RETRO_MEMORY_SAVE_RAM` = P1, `RETRO_MEMORY_GAMEBOY_1_SRAM` … for the rest |
| `retro_unload_game` | Free everything | Tear down instances + coordinator |

Core options via `RETRO_ENVIRONMENT_GET_VARIABLE` (upstream's
`libretro_core_options.h` already has the infra to extend): `splitscreen_players`
(reload-gated), `splitscreen_layout` (`2x1`/`2x2`/`1x2`), `splitscreen_audio`
(`player 1`/`2`/`3`/`4`/`mixed`), plus all upstream mGBA options per-core
(applied to every instance; per-player overrides later if wanted).

Performance/serialization niceties already in libretro.h if wanted later:
`RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL`, `SET_SUPPORT_ACHIEVEMENTS`,
`GET_CLEAR_ALL_THREAD_WAITS_CB` (relevant if we ever move the frame loop to a
worker thread — do **not** in v1; RetroArch calls `retro_run` from its own
audio/video-driven loop and cores are expected to be synchronous).

## Requirements to be an OFFICIAL core (the distribution pipeline)

All verified 2026-09-28 from `docs.libretro.com` (Developing Libretro Cores →
"Add your core to Libretro infrastructure"), the Clownacy write-up (Aug 2025,
AREN — first-hand), `libretro/mgba` and `libretro/tgbdual-libretro` in-tree
examples, and `libretro-super/dist/info/mgba_libretro.info`:

1. **`.info` file in libretro-super** (`libretro-super/dist/info/<name>_libretro.info`):
   display name, `corename`, `license` (ours: `MPLv2.0`), `supported_extensions`,
   `systemname/systemid`, feature flags (`savestate = "true"`,
   `savestate_features = "deterministic"` — see caveat in Edge cases,
   `input_descriptors`, `core_options`, `load_subsystem = "true"` — TGB Dual
   does this; upstream mgba's info has `load_subsystem = "false"` because its
   `retro_load_game_special` stub returns false), firmware entries.
   Test locally: drop the file into RetroArch's `libretro_info_path`, delete
   `core_info.cache`, load the core, check Information → Core Information.
2. **`.gitlab-ci.yml` at the root of the core's repo** (copy `libretro/mgba`'s —
   it builds mGBA's CMake with `CORE_ARGS: -DLIBMGBA_ONLY=ON -DBUILD_LIBRETRO=ON`
   across windows/linux/osx/android/ios/vita/ps2/wii-u/3ds templates). No
   template exists for this file; you copy a similar core's and trim.
3. **Repository mirrored onto libretro's GitLab instance** — the buildbot only
   builds repos mirrored there. This is the undocumented step: a maintainer
   (historically Warmenhoven) adds your repo to the GitLab crawl list. Do this
   via libretro's Discord `#programming`. Nightlies then appear in RetroArch's
   Online Updater → Core Downloader.
4. **libretro-docs entry** (`libretro/docs#adding-a-new-core`): one-line list
   edits plus a core doc page from their annotated template.
5. Optional polish: RetroArch-assets XMB icon, libretro-database entries
   (GBA DB already exists — only needed if we want playlist content mapping).

There is no formal PR template or review board; the practical bar is "builds
green on the buildbot templates + a maintainer willing to mirror it." Upstream
mGBA's own libretro build is the safest possible recipe to copy.

**Licensing note:** the core inherits mGBA's MPL-2.0 (the API itself is MIT;
RetroArch is GPLv3 but cores are independent works — mGBA's core already ships
in RetroArch's Core Downloader, so a fork by definition can too).

## This fork's value-add vs upstream's existing core

Upstream's `src/platform/libretro/libretro.c` (1,414 lines, `BUILD_LIBRETRO`
CMake target `${BINARY_NAME}_libretro`) is a single-instance core with **zero**
references to the lockstep driver — the linking machinery exists upstream but no
core frontend ever drives more than one core with it. Our fork adds:

1. The **FS assist + gated deadlock kick** in `src/gba/sio/lockstep.c`
   (`GBASIOLockstepCoordinatorSetFSuppressed/SetFSArmed`, the round-boundary
   kick) — this is what gets Four Swords past the linking screen.
2. A proven **sequential (single-thread) driving model** for N cores
   (`emulation.rs`): budgeted cooperative stepping, per-player sleep flags,
   frame snapshots taken at frame-counter change.
3. Product decisions: player-count selection, layout options, audio routing
   (the web app's per-seat audio is the same problem the core's
   `splitscreen_audio` option solves).

## Design in one paragraph

New directory `src/platform/libretro-splitscreen/` (fork-local; upstream's
`src/platform/libretro/` stays untouched so rebases stay cheap — same principle
as the rest of the fork). One shared library exporting the full `retro_*`
surface. `retro_load_game` = single player (a lightly-wrapped upstream core);
`retro_load_game_special` with a new subsystem type
`RETRO_GAME_TYPE_GBA_LINK_2P/3P/4P` (0x201/0x202/0x203 — TGB Dual uses 0x101,
documented ranges avoid collisions) = the splitscreen path: construct N
`struct mCore`s + one `GBASIOLockstepCoordinator` + N `GBASIOLockstepDriver`s,
wire the Rust-proven sleep-flag model in C, then per `retro_run()`: read N
RetroPads (port p → player p), cooperatively advance every player one video
frame (the `emulation.rs` budgeted-stepping loop, ~50 lines of C), composite
the N most-recent per-player snapshots into one 480×320 (2P) or 960×640 (2×2,
4P) RGB565 frame, mix audio per the `splitscreen_audio` option, call
`video_cb`/`audio_batch_cb` once each. SRAM: P1 on `RETRO_MEMORY_SAVE_RAM`,
P2–P4 on TGB Dual's per-instance memory IDs. Save states serialize the
coordinator + all N cores into one blob (mirrors the DUALSTATE format).

## Detailed design

### 1. Build plumbing

- `CMakeLists.txt`: new option `BUILD_LIBRETRO_SPLITSCREEN` (default OFF)
  building `mgba_splitscreen_libretro` from `src/platform/libretro-splitscreen/*.c`
  with the same defines as upstream's libretro target
  (`__LIBRETRO__;COLOR_16_BIT;COLOR_5_6_5;DISABLE_THREADING;MINIMAL_CORE=2;ENABLE_VFS`).
- Also provide the libretro-super-style **Makefile** at the fork root gated by
  `platform := unix|win|...` because some buildbot console templates are
  Makefile-only; mGBA's CMake recipe is primary (the `.gitlab-ci.yml` in
  `libretro/mgba` proves the CMake path covers all the targets we care about).
- `src/platform/libretro-splitscreen/` carries its own vendored
  `libretro_common/` (or symlink convention used by libretro ports) so the
  buildbot never clones a second repo.

### 2. Instance management (`instances.c/.h` in the new dir)

- `struct sp_instance`: `struct mCore* core; struct mAVStream stream;
  struct GBASIOLockstepDriver driver; uint16_t keys; uint32_t* videoBuf;
  int16_t audioBuf[...]; uint32_t frameCounter; bool asleep;`
- `sp_create(n, roms[])`: `core = mCoreFind(...)`, `core->init()` FIRST
  (**repo gotcha**: before `mCorePreloadFile`), load ROM from the
  `retro_game_info.data` via `VFileMemChunk`, `core->desiredVideoSize` →
  allocate `videoBuf`, install `mAVStream` (audio drain + rate reporting),
  set config options from core variables, then `GBASIOLockstepDriverCreate`
  + `GBASIOLockstepCoordinatorAttach` for each, then `core->reset()`
  (**repo gotcha**: attach before reset or games see the cable mid-boot).
- The coordinator lives in `instances.c` as a single static (libretro allows
  global state), initialized in `retro_init`, deinited in `retro_deinit`.

### 3. Frame loop (`retro_run`)

Port of `emulation.rs` lines ~820–875 to C:

```c
static void run_one_frame(void) {
    input_poll_cb();
    for (p = 0; p < nPlayers; ++p) keys[p] = read_retropad(p); /* keymap order */
    /* cooperative stepping: every player advances one video frame,
       switching players whenever one sleeps on the link */
    int32_t budget[MAX_PLAYERS]; bool progress = true;
    for (p = 0; p < nPlayers; ++p) budget[p] = FRAME_CYCLES;
    while (progress) {
        progress = false;
        for (p = 0; p < nPlayers; ++p) {
            if (budget[p] <= 0 || sleeping[p]) continue;
            progress = true;
            before = mTimingCurrentTime(&cores[p]->timing);
            cores[p]->runLoop(cores[p]);
            budget[p] -= mTimingCurrentTime(&cores[p]->timing) - before;
            if (frame_counter changed) snapshot(p); /* full buffer copy */
        }
    }
    composite_video();   /* blit snapshots per layout option */
    video_cb(videoOut, W, H, W * 2);
    flush_audio();       /* mAudioBufferRead per player, mix, audio_batch_cb */
}
```

- `sleeping[p]` is set/cleared by the `mLockstepUser` `sleep`/`wake` callbacks
  (Rust: `lockstep_sleep`/`lockstep_wake`). Skip-while-asleep is what makes
  the threaded lockstep driver work on one thread.
- Keep the **step cap** (Rust uses 100,000 steps) as a livelock guard: if the
  while loop can't finish a frame, log once and move on rather than hang the
  frontend's audio thread.
- Frame counter: `core->currentVideoSize` changes only on GB/GBC SGB modes;
  for GBA 240×160 is fixed — snapshot per player into per-player buffers, then
  `composite_video()` memcpy/row-scales into the shared output buffer.

### 4. Video composition

- 2P: `480×160` side-by-side (or `240×320` stacked) — **do not** letterbox
  individually; let RetroArch's own shader/aspect pipeline handle scaling.
- 4P: `480×320` quadrant; 3P: 480×320 with one quadrant black (simplest) or
  480×160 triple — default quadrant for A/V consistency.
- Option `splitscreen_layout` re-composites at runtime by re-calling
  `RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO` (upstream's core already does this
  dance for GB model changes).
- Bitmap labels ("P1"…) are NOT drawn into the framebuffer by default
  (shaders/purists); a core option `splitscreen_labels` can add a tiny 8px
  corner tag later.

### 5. Audio

- Per `retro_run`, read each player's `mAudioBuffer` (~533 frames @32,768 Hz),
  apply `splitscreen_audio` (mix = saturating sum / n, or single-player
  passthrough), then one `audio_batch_cb(mix, frames)`.
- GB/GBC mixed sessions report per-instance rates (the web app hit this:
  GB rate ≠ GBA 32768 — read `stream->audioRateChanged` per instance and
  resample on mismatch; GBA-only v1 can assert 32768).

### 6. Subsystem manifest + memory

```c
static const struct retro_subsystem_memory_info gba1_mem[] =
    { { "sav", RETRO_MEMORY_SAVE_RAM }, { "sav", RETRO_MEMORY_GAMEBOY_1_SRAM } };
/* … gba2_mem/gba3_mem/gba4_mem with ids 1/2/3 … */
static const struct retro_subsystem_rom_info gba_roms[] = {
    { "Player 1", "gba", false, false, false, gba1_mem, 2 }, … };
static const struct retro_subsystem_info subsystems[] = {
    { "GBA Link 2 Player", "gba_link_2p", gba_roms, 2, RETRO_GAME_TYPE_GBA_LINK_2P },
    { "GBA Link 3 Player", "gba_link_3p", gba_roms, 3, RETRO_GAME_TYPE_GBA_LINK_3P },
    { "GBA Link 4 Player", "gba_link_4p", gba_roms, 4, RETRO_GAME_TYPE_GBA_LINK_4P },
    { NULL } };
```

- Also set `RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO` in `retro_set_environment`
  so RetroArch's playlist flow offers "Load 2-Player Link Content" style
  pickers (this is how TGB Dual's flow surfaces).
- `retro_get_memory_data(RETRO_MEMORY_SAVE_RAM)` returns player 1's SRAM;
  `RETRO_MEMORY_GAMEBOY_n_SRAM` the others (TGB Dual's pattern, including its
  "num_memory must cover both arrays or the frontend never asks" comment —
  **copy that comment's lesson**: declare `n` memory entries per ROM = every
  array's max length, or saves get dropped on exit).

### 7. Single-ROM quick path (player-count option)

`retro_load_game` (plain) ALSO accepts a core option `splitscreen_players = 1|2|3|4`
with the *same ROM per player* (reload-gated, like TGB Dual's
`tgbdual_gblink_enable`), because most link games are bought once. The
subsystem path exists for genuinely different ROMs. Both paths converge on the
same instance manager. This mirrors TGB Dual's `MODE_SINGLE_GAME_DUAL` vs
`MODE_DUAL_GAME`.

### 8. Fork-specific knobs to carry over

- `GBASIOLockstepCoordinatorSetFSuppressed(coord, true)` as core option
  `splitscreen_fs_assist = on|off` (default **off** to match the desktop/web
  behavior; the driver's own hard-sync is the real fix).
- The **kick** is self-gated on FS + freeze signature inside `lockstep.c` and
  needs no host arming — it just works, no core option needed (matches the
  shipped app).
- Player-count UI: no JS here; the frontend's "how many players" is the
  subsystem choice / core option, which is the whole point.

## Edge cases checklist

- [ ] `retro_serialize_size()` must never increase between load and unload:
      snapshot the maximum at load (N × `mCore` max state + coordinator) and
      always pad to it. (RetroArch rewind depends on this.)
- [ ] `savestate_features = "deterministic"` (for netplay/rewind) requires the
      coordinator to serialize cleanly; the DUALSTATE work already proved N-core
      serialization, and the round-4 fix (`GBASIOLockstepDriverReset` after
      import) is in-tree — but **verify**: restore into a mid-transfer state
      (the "cannot reach gameplay from a mid-handshake state" to-do lesson).
      If unstable, set `savestate_features = "nondeterministic"` in the .info
      and iterate (TGB Dual ships netplay-with-caveats the same way).
- [ ] Loading a 4-player subsystem with only 2 ROMs given → fail loudly, return
      `false` from `retro_load_game_special` (frontends handle that).
- [ ] Same ROM for all 4 via quick path: mGBA's `mCoreLoadFile` must be given 4
      *separate* `VFile`s (save data must not alias! per-instance
      `VFileMemChunk` copies, one per player).
- [ ] GB/GBC ROMs in the 4-player GBA subsystem: reject (platform mismatch);
      GB/GBC 2-player comes later as its own subsystem type (`gb_link_2p`
      equivalent exists upstream in TGB Dual only for GB — our GB coordinator
      path could be phase 5).
- [ ] BIOS handling: honor `retro_system_info.need_fullpath=false` (memory
      loading) for soft-patch compat; upstream already does this.
- [ ] Frame-time discipline: never sleep(); the frontend controls pacing — the
      budgeted-stepping loop must not call any timing/sleep APIs.
- [ ] Thread safety: no threads at all in v1 (`DISABLE_THREADING` is already
      set for the libretro build); if a worker thread is ever added, core calls
      must stay on the caller's thread (docs' thread-safety note).
- [ ] Android/ARM: the fork's lockstep.c FS-assist block compiles clean at
      `-O2` on GCC — verify no strict-aliasing UB was introduced (it does
      struct writes only, but check with `-Wstrict-aliasing`).
- [ ] The 100k-step cap can leave players at unequal frame counts → the
      composite must use the *last completed* frame per player (the snapshot
      design), never an intermediate buffer.

## Verification plan (mirrors the harness-first culture)

1. `cmake -DBUILD_LIBRETRO_SPLITSCREEN=ON` builds `mgba_splitscreen_libretro.so`;
   `nm -D` shows all `retro_*` symbols exported.
2. Headless frontend first: **retroarch-headless** or `libretro-samples` runner
   with the linktest ROM (`mgba-splitscreen/linktest/`) — all 4 units reach
   `LINK ACTIVE - 4 PS` (the same acceptance bar `tests/linktest_4p.rs` uses).
   Assert via the core's log callback, not screens.
3. RetroArch desktop: load linktest via 4-player subsystem → 4 quadrants show
   the instrument ROM; RetroPad ports 0-3 map to P1-P4 (check the input
   descriptor overlay). Repeat with Mario Kart 2P: both screens race, item
   boxes hit at the same time (visual parity check with the web app).
4. FS end-to-end (the fork's crown jewel): 4× Four Swords via subsystem →
   import-free fresh boot → all 4 reach gameplay (the 2026-09-08 2P + current
   4P state is the benchmark; the core inherits `lockstep.c` as-is so this is
   expected to behave like the desktop app).
5. Save states: save at 4P gameplay → reload → games continue in lock (also
   feeds the `.info` `savestate_features` decision). Battery saves: quit
   RetroArch, relaunch, saves reattach per player (verify the
   `num_memory` lesson above).
6. Fast-forward + run-ahead: if run-ahead produces link desyncs (it rewinds
   state copies), document that run-ahead must stay OFF for linked sessions
   (RetroArch netplay docs flag the same class of issue) — do not chase it.
7. Only then: `.gitlab-ci.yml` → buildbot green on linux/windows/osx →
   GitLab mirror request → nightly.

## Suggested build order

- **Phase 1 — skeleton core (days):** copy `src/platform/libretro/libretro.c`
  into `libretro-splitscreen/`, strip sensors/camera/low-pass, rename
  `display_name` ("mGBA Splitscreen"), get it loading a single ROM in RetroArch.
  Zero fork logic yet — this de-risks all the boring plumbing.
- **Phase 2 — N instances, no link:** instance manager + per-player input +
  side-by-side composite + audio options, with lockstep attach *compiled out*.
  Proves the frame loop and video math.
- **Phase 3 — the link:** wire `GBASIOLockstepCoordinator` + drivers +
  sleep-flag model; linktest ROM must pass 4P; Mario Kart 2P races.
- **Phase 4 — subsystem + options:** `retro_load_game_special` for 2/3/4,
  core options (layout, audio, FS assist), save-state/SRAM plumbing, .info file.
- **Phase 5 — submission:** `.gitlab-ci.yml`, libretro-super PR, docs PR,
  GitLab mirror ask, nightly verification. (Optional parallel: hook the core
  into this repo's `release.yml` as an extra artifact so testers get .so/.dll
  builds before official distribution.)
- **Phase 6 (later) — cross-platform polish:** GB/GBC 2P subsystem,
  per-player core options, achievements-compat check, Rumble (upstream core
  has it) per player.

## Parked risks (not v1)

- **RetroArch "netplay" over a multi-instance core** is uncharted: rewind-
  based netplay would serialize the whole coordinator blob — it may work or
  may desync; explicitly unsupported at first, revisit after determinism
  testing.
- **Run-ahead** (RetroArch latency feature) interacts badly with multi-core
  lockstep; needs an environment `SET_MINIMUM_AUDIO_LATENCY`-style opt-out or
  a documented "turn it off" note.
- **Strict-aliasing / UB in the FS assist** blocks console (3DS/Vita) builds —
  the fork only ever shipped it on desktop/web so far; fix before requesting
  those buildbot targets.
- **Performance on consoles** (3DS: 4 GBA cores + 60 fps is a big ask; PSP:
  worse). Target desktop + Android/iOS first; do not gate official-core
  submission on console targets — TGB Dual ships 2 GB cores everywhere, but 4
  GBA cores is heavier.
- **Per-player hotkey** (e.g. pause one player) has no RetroArch concept —
  skip entirely (frontend pauses the whole core).
