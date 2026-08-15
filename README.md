# DualBoy

DualBoy is a **split-screen Game Boy Advance emulator**: run multiple GBA instances
side by side, linked together over a virtual link cable, so two to four players can
play multiplayer GBA games (trading, link battles, co-op, etc.) on a single machine —
each player gets their own screen and their own controls.

It is built on top of the excellent [mGBA](https://mgba.io/) core and is a fork of
[mGBA](https://github.com/mgba-emu/mgba) (`Spuds0588/mgba-splitscreen`), which provides
the emulation engine, accuracy, and the lockstep link-cable synchronization used to keep
the instances in perfect sync.

## Architecture

- **`libmgba` (C)** — the mGBA core, compiled as a static library. Does all emulation,
  including the lockstep link-cable driver that keeps instances in lockstep.
- **Rust backend** (`DualBoy/src-tauri`) — wraps `libmgba` (via `bindgen`), manages the
  emulation instances, runs the frame loop, and serves frames + input over a WebSocket.
- **Frontend** (`DualBoy/src`) — a lightweight HTML/JS canvas UI that renders each
  instance's screen and maps keyboard input to GBA buttons.

The backend and frontend communicate over WebSocket, so the same frontend can be used
both inside the Tauri desktop app and in a plain web browser.

## Features

- Split-screen play: two (desktop) or up to four (web) GBA instances, each with its own screen.
- Virtual link cable: instances stay in perfect, drop-free synchronization via mGBA's
  lockstep link-cable support — trade and battle across instances just like real hardware.
- Load one ROM and it runs on every instance simultaneously.
- Per-player keyboard mapping (laptop-optimized layout).
- Save import/export per instance, or as a set across all running instances.
- Web demo: play in the browser with no install.

## Controls

### Player 1 (left hand cluster)

| GBA button | Key |
|------------|-----|
| D-Pad      | `W` `A` `S` `D` |
| A          | `K` |
| B          | `J` |
| L          | `H` |
| R          | `L` |
| Start      | `Enter` |
| Select     | `Backspace` |

### Player 2 (right hand / arrows cluster)

| GBA button | Key |
|------------|-----|
| D-Pad      | Arrow keys |
| A          | `M` |
| B          | `N` |
| L          | `V` |
| R          | `B` |
| Start      | `P` |
| Select     | `O` |

## Building

The desktop app is a [Tauri](https://tauri.app/) v2 project. Prerequisites:

- Rust toolchain (`cargo`, `rustc`)
- `cmake`, `clang` (for building `libmgba` and generating `bindgen` bindings)
- Tauri v2 system dependencies (WebKitGTK on Linux, etc.)

```bash
cd DualBoy
npm install
npm run tauri dev        # development run
npm run tauri build      # production build
```

The first build compiles all of `libmgba` from source, which takes a few minutes and
several GB of RAM; subsequent builds are incremental.

## Project status

See [`PROJECT_LOG.md`](PROJECT_LOG.md) for the current state of the project, what's
implemented, and what's in progress. This is kept up to date so future sessions can
pick up where the last one left off.

## License & attribution

DualBoy's original code is distributed under the same terms as mGBA, the
[Mozilla Public License version 2.0](https://www.mozilla.org/MPL/2.0/).

The emulation core is **mGBA**, Copyright © 2013 – 2026 Jeffrey Pfau.
mGBA is licensed under the [Mozilla Public License version 2.0](https://www.mozilla.org/MPL/2.0/).
A copy of the license is in the distributed [`LICENSE`](LICENSE) file.
See the upstream repository at <https://github.com/mgba-emu/mgba>.

mGBA contains the following third-party libraries:

- [inih](https://github.com/benhoyt/inih), Copyright © 2009 – 2020 Ben Hoyt, BSD 3-clause license.
- [LZMA SDK](http://www.7-zip.org/sdk.html), public domain.
- [MurmurHash3](https://github.com/aappleby/smhasher) implementation by Austin Appleby, public domain.
- [getopt for MSVC](https://github.com/skandhurkat/Getopt-for-Visual-Studio/), public domain.
- [SQLite3](https://www.sqlite.org), public domain.

If you are a game publisher and wish to license mGBA for commercial usage, please email
[licensing@mgba.io](mailto:licensing@mgba.io) for more information.
