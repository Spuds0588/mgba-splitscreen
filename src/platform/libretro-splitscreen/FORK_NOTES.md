# mGBA Splitscreen libretro core — fork notes

This core is a fork of upstream mGBA's libretro core
(`src/platform/libretro/`, BUILD_LIBRETRO), carrying the mgba-splitscreen
link work. It is intentionally a **multiplayer-only** core:

- **Sessions: 2-4 only.** "Players per ROM" defaults to 2 (options 2/3/4).
  There is no 1-player mode; for single-player play use upstream mGBA's
  core ("mGBA" in the Core Downloader). This keeps the core honest about
  its use case: linked-session multiplayer with the splitscreen views, not
  a general-purpose mGBA replacement.
- The `mgba_forkstock_libretro` build (BUILD_LIBRETRO=ON from this fork's
  tree) is a **compatibility test instrument** for RetroArch home-menu
  flows (content loading, memory-map quirks with the fork's source), not a
  shipping product. It exists because the distro ships no mGBA core at
  all; it may be removed once the splitscreen core passes the same flows.

## Save-data compatibility with the upstream mGBA core (plan)

Goal: a user playing solo in upstream mGBA (or the desktop app) can bring
their battery save into a splitscreen session (Shining Soul II character),
and a P1 seat can go back to upstream mGBA.

The on-disk format is already compatible in practice:

- Both cores allocate `GBA_SIZE_FLASH1M` (1 MiB) battery buffers and hand
  the frontend an opaque `RETRO_MEMORY_SAVE_RAM` blob via
  `retro_get_memory_data/size`. mGBA battery saves are type-tagged and
  sized by the emulator at write time; `loadSave`/`GBASavedataInit*`
  accept the real (smaller) file inside the buffer. So an upstream
  `foo.srm` written by upstream mGBA loads into a splitscreen seat as-is.
- Our P1 seat is `RETRO_MEMORY_SAVE_RAM` (frontend filename
  `<rom>.srm`, same as upstream mGBA), so **P1 interops with zero
  conversion today**. P2-P4 use subsystem memory ids 0x100-0x102
  (`<rom>.sav2/3/4`, TGB Dual convention).

Known divergence (harmless on read, wasteful on write, worth fixing):

- `sp_memory_size(RETRO_MEMORY_SAVE_RAM)` returns the full 1 MiB buffer,
  so frontends write a 1 MiB file even for a 64 KiB SRAM game. Upstream's
  core returns the real per-game size (`GBASavedataSize`) for GBA.
- Follow-up: query each player's core for its real save size and return
  that from `sp_memory_size` (clamped to the buffer). Truncating to the
  real size is safe for reads by both cores; keeping the 1 MiB buffer as
  the backing store avoids ABI churn.

Future UX (not yet built): a "copy solo save into seat N" flow — desktop
app file picker or RetroArch-side manual copy of `foo.srm` next to the
content as `foo.sav2` before a subsystem load.

## Rebuild reminder

The user-facing bug that motivated this note: an installed
`mgba_splitscreen_libretro.so` predating commit 88831a7ef (view modes +
core options) showed no Core Options and a single left-aligned session.
`build-libretro-sp/` had been wiped by disk cleanup, so the artifact could
not be refreshed in place. Always rebuild and reinstall after pulling:

```
cmake -S . -B build-libretro-sp \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS_RELEASE='-O2 -DNDEBUG -DCOLOR_16_BIT -DCOLOR_5_6_5' \
  -DLIBMGBA_ONLY=ON -DBUILD_LIBRETRO_SPLITSCREEN=ON
cmake --build build-libretro-sp --target mgba_splitscreen_libretro -j
cp build-libretro-sp/mgba_splitscreen_libretro.so ~/.config/retroarch/cores/
cp src/platform/libretro-splitscreen/mgba_splitscreen_libretro.info \
   ~/.config/retroarch/cores/
```

The `-DCOLOR_16_BIT -DCOLOR_5_6_5` flags are mandatory: the core
advertises RGB565 and the pixel-verification harness depends on them.
