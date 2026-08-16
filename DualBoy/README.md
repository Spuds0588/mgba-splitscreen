# DualBoy

A **split-screen GBA emulator** that runs 2–4 Game Boy Advance instances side by side
and links them over a **virtual link cable**, so two to four players can trade,
battle, or co-op in link-cable games (e.g. *The Legend of Zelda: Four Swords*) on a
single machine — one ROM, one window, one keyboard/gamepads.

DualBoy is **not** its own emulator core. It is a host application built on
**[mGBA](https://mgba.io/)** (`mgba-emu/mgba`), using mGBA's `libmgba` and its
lockstep multiplayer link implementation. All emulation and link-cable work is
mGBA's; DualBoy adds the multi-instance orchestration, the synchronized frame
streaming, input mapping, save management, and the UI.

## Repo layout

- `DualBoy/` — the application (a Tauri v2 app; the same frontend is served by a
  standalone web server for browser play).
- Everything else — upstream mGBA source, compiled into `libmgba` by
  `DualBoy/src-tauri/build.rs`.

## How it works

Each player gets their own mGBA `mCore` (GBA) instance. The instances are attached to
one `GBASIOLockstepCoordinator` via `GBASIOLockstepDriver`s **before** the ROM boots,
so link-cable games detect the cable at boot. Emulation runs every instance at ~60 FPS
and the lockstep link keeps them in sync; video is streamed independently (default
30 FPS) so lowering the video rate never slows the game or breaks the link.

## Run it

```bash
cd DualBoy/src-tauri

# Desktop app (a window on your display)
cargo build --release
./target/release/dualboy

# Browser demo (open http://127.0.0.1:8080)
cargo run --release --bin dualboy-web -- --players 4 --fps 30
```

**Always use `--release`.** The emulation core is built `-O3` either way, but the Rust
frame pipeline and UI glue are ~10x slower in a debug build (the debug desktop binary
is ~223 MB vs ~11 MB release).

Load a ROM with **File → Load ROM…** (desktop: native file dialog; browser: file
picker). `Test Roms/` (gitignored) holds the owner's legal ROMs for testing.

## Controls

| | Player 1 | Player 2 |
|---|---|---|
| D-pad | W A S D | Arrow keys |
| A / B | K / J | M / N |
| L / R | H / L | V / B |
| Start / Select | Enter / Backspace | P / O |

The UI is mouse-driven (a top menu bar), so game keys never fight the UI. Players
3–4 are keyboard/gamepad targets for the browser demo (see the backlog in
`PROJECT_LOG.md`).

## Saves

- **Saves → Export/Import Save P*n*** — one instance's `.sav` (mGBA-compatible).
- **File → Export/Import All Saves…** — a single `.dualbysave` bundle of every
  instance's save, so a party can be moved between machines.

## Performance

The link-critical constraint is **emulation speed**, and mGBA is fast: on this
machine 4 synchronized instances of *Shining Soul II* (one of the heavier GBA games)
emulate at ~4,000 FPS on one thread (0.24 ms/frame), so the 60 FPS target has huge
headroom. The UI path is the real budget, so DualBoy:

- streams frames as RGBA8888 and feeds them straight into `putImageData` (no
  per-pixel decode in JS),
- decouples video FPS from emulation (`--fps`, default 30),
- is built with an optimized Rust release profile.

`cargo run --release --bin bench -- <rom.gba> [players]` measures pure emulation
speed in isolation.

## Testing

```bash
cd DualBoy/src-tauri
cargo test --release            # 128 unit + 2 smoke tests (needs Test Roms/)
```

See `DualBoy/TEST_INSTRUCTIONS.md` and `DualBoy/scripts/README.md` (headless
gameplay drivers) for more.

## Credits

Emulation and link-cable multiplayer: **[mGBA](https://mgba.io/)** by endrift and
contributors. DualBoy is a thin host around `libmgba` and would not exist without it.
