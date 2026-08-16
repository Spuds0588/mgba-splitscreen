# DualBoy: Testing Instructions

Follow these steps to test the current build of the DualBoy split-screen GBA emulator.

## 1. Launch the app

Build and run a **release** binary (the debug build is ~10x slower):

```bash
cd DualBoy/src-tauri
cargo build --release
./target/release/dualboy          # desktop window on your display
# or, for the browser demo:
./target/release/dualboy-web --players 2 --fps 30   # http://127.0.0.1:8080
```

## 2. Load a game

1. Click **File → Load ROM…** (desktop: native file dialog; browser: file picker).
2. Pick a `.gba` from `Test Roms/`.
3. The screens tile into a video-call-style grid (2/3/4 equal cells) and every
   instance shows the same game.

## 3. Control mapping (laptop optimized)

| | Player 1 | Player 2 |
|---|---|---|
| D-pad | W A S D | Arrow keys |
| A / B | K / J | M / N |
| L / R | H / L | V / B |
| Start / Select | Enter / Backspace | P / O |

The UI is a mouse-driven top menu bar — game keys never navigate it, so there are no
keyboard conflicts. Click the game area once before playing so key focus lands there.

## 4. Verification checklist

- [ ] **Layout**: menu bar on top; screens split into 2/3/4 equal cells with no scrolling.
- [ ] **Speed**: smooth animation, input responds without lag. (Expect ~30 FPS video;
      emulation itself runs far faster.)
- [ ] **Sync**: both instances stay in lockstep (e.g. identical cutscenes; no
      "turn the power OFF/ON" multi-pak error).
- [ ] **Multiplayer**: in *Four Swords*, boot → `CHOOSE A FILE` → create a save →
      `CHOOSE A GAME` → Four Swords → both players reach the connection screen.
- [ ] **Saves**: **Saves → Export/Import Save P1/P2** and **File → Export/Import All
      Saves…** round-trip.

## 5. Reporting feedback

Report issues with:
1. **Performance** — stutter or slow frames (and which game).
2. **Sync** — disconnect or drift between instances.
3. **Controls** — key mapping comfort or focus conflicts.
