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
  instance's screen and maps keyboard/gamepad input to GBA buttons.

The backend and frontend communicate over WebSocket, so the same frontend can be used
both inside the Tauri desktop app and in a plain web browser.

## Features

- **1–4 split-screen players** on one machine, each with its own screen and controls.
  Pick the player count from the **Players** menu or as a step when launching from the
  game library (game → players → start).
- **Virtual link cable**: instances stay in perfect, drop-free synchronization via mGBA's
  lockstep link-cable support — trade and battle across instances just like real hardware.
- **Game library launcher** (**File → Games Library**): Recent games (persisted) plus ROM
  folders you add, with box art (local sibling images or an online fallback) and
  controller/keyboard navigation.
- **Save states**: quick save (F5) / quick load (F7) capture *all* players together;
  both hotkeys are remappable. Battery saves next to a ROM (`game.sav`, `game.sa2`, …)
  are auto-loaded.
- **Controller support** (Gamepad API): controller slot #1 → P1, #2 → P2, etc., with
  per-player button re-mapping and remappable global hotkeys (turbo, save/load, pause)
  — all via **Controls → Remap Hotkeys…**, persisted in the browser.
- **Pause menu**: pausing (Escape) freezes all players at once and pops a
  controller-navigable menu (resume / save / load / players / library / quit ROM).
- **Video-call-style view modes** (**View** menu): Grid, Speaker (1 big + smalls),
  Focus (single screen), and Overlay/PiP, cycled with F8/F9 (remappable). Background
  image, per-player outline toggles, and a toggleable debug log are all in the View menu.
- **Turbo mode** (Q): fast-forward past 60 fps for grinding through menus/animations.
- **Save import/export** per instance or as a set across all running instances.
- **Web demo**: play in the browser with no install (frames + audio stream over WebSocket).
  The static UI shell is hosted on
  [GitHub Pages](https://spuds0588.github.io/mgba-splitscreen/); to actually play,
  grab a `dualboy-web` binary from the latest
  [Release](https://github.com/Spuds0588/mgba-splitscreen/releases) and open its
  `http://127.0.0.1:8080` address.

## Controls

Per-player layouts are fully remappable (keyboard + gamepad) from **Controls** in the
app, so the tables below are just the defaults.

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

### Player 3 (`T`/`G`/`F`/`R` cluster) and Player 4 (`I`/`Q`/`C`/`E` cluster)

Defaults are listed in **Help** in the app; every key for every player can be remapped.

### Global hotkeys (remappable)

| Action | Default |
|--------|---------|
| Turbo | `Q` |
| Quick save (all players) | `F5` |
| Quick load (all players) | `F7` |
| Pause / resume (all players) | `Escape` |
| Cycle view mode | `F8` |
| Cycle focus player | `F9` |

## Building

The desktop app is a [Tauri](https://tauri.app/) v2 project. Prerequisites:

- Rust toolchain (`cargo`, `rustc`)
- `cmake`, `clang` (for building `libmgba` and generating `bindgen` bindings)
- Tauri v2 system dependencies (WebKitGTK on Linux, etc.)
- Node.js (`npm`) for the Tauri CLI

```bash
cd DualBoy
npm install
npm run tauri dev        # development run
npm run tauri build      # production build (bundles .deb/.AppImage on Linux, etc.)
```

The first build compiles all of `libmgba` from source, which takes a few minutes and
several GB of RAM; subsequent builds are incremental.

### Web server build (browser play)

A standalone server streams frames + audio to any browser — no install needed:

```bash
cd DualBoy/src-tauri
cargo run --bin dualboy-web -- --players 4   # 2, 3, or 4 players
# then open http://127.0.0.1:8080 in a browser
```

Load a ROM via the file picker; screens, controls, save states, and audio work the same
as the desktop app.

## Play in the browser (web version)

The web UI is hosted on
**[GitHub Pages](https://spuds0588.github.io/mgba-splitscreen/)** — you can open the
link and see the launcher in your browser with no install. One important caveat: the
hosted page is the *interface only*. GBA emulation runs in the `dualboy-web` backend
(a Rust server that hosts the emulator cores and streams frames + audio to your
browser), and browsers can't run that server on someone else's machine — it needs a
host to emulate on.

To actually play, download the `dualboy-web-linux-x64` binary from the
[Releases](https://github.com/Spuds0588/mgba-splitscreen/releases) page, run it, and
open <http://127.0.0.1:8080> (the backend runs on any Linux machine; for Windows/macOS
use the desktop app):

```bash
./dualboy-web --players 2    # 2, 3, or 4 linked players
# then open http://127.0.0.1:8080 in any browser on your network
```

Frames, audio, controls, save states, and the game library all work exactly as in the
desktop app. (A future "hosted multiplayer" mode could relay a host's frames to remote
players over WebRTC — see the roadmap.)

## Beta releases

Beta builds are produced from version tags (`vX.Y.Z-beta.N`) by the
[release workflow](.github/workflows/release.yml). It builds and uploads:

| Platform | Artifact |
|----------|----------|
| Linux | `dualboy_*.deb` + `dualboy_*.AppImage` |
| macOS | `dualboy_*.dmg` |
| Windows | `dualboy_*.msi` (installer) |
| Web | `dualboy-web-linux-x64` static server binary (self-host the backend; Windows/macOS users can run the desktop app) |

The web UI shell is also always live on
[GitHub Pages](https://spuds0588.github.io/mgba-splitscreen/) (auto-deployed from
`master` by [.github/workflows/pages.yml](.github/workflows/pages.yml)).

To cut a beta:

```bash
# bump the version in DualBoy/src-tauri/tauri.conf.json and DualBoy/package.json,
# commit, then tag and push (the workflow uploads to a GitHub Release):
git tag v0.1.0-beta.1
git push origin v0.1.0-beta.1
```

To build a platform's bundle locally instead of via CI, run `npm run tauri build` on
that platform (each platform must build its own bundle — no cross-compilation).

The web server binary is also published per-platform; it needs no install and serves the
browser frontend from `DualBoy/src` (see `.freebuff/run.md` for the runbook).

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
