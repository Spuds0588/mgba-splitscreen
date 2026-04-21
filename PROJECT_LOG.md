# DualBoy Project Log

## Context
We are building "DualBoy", a split-screen GBA emulator using Tauri (Rust backend, JS frontend) and `libmgba` (C library).
The project starts from a fork of mGBA: `https://github.com/Spuds0588/mgba-splitscreen.git`.

## Status: 2026-04-21
- [x] Cloned `mgba-splitscreen` into `/home/coreyb/Coding Projects/Applications/mgba-splitscreen`.
- [x] Reviewed PRD (`Applications/split_screen_gba_project.md`).
- [x] Initialized Tauri project `DualBoy` in `/home/coreyb/Coding Projects/Applications/DualBoy`.
- [x] Setup `build.rs` with `libmgba` compilation and `bindgen`.
- [x] Implemented `GbaInstance` wrapper in Rust.
- [x] Setup WebSocket server for frame streaming.
- [x] Basic UI with Dual Canvases and ROM loader.
- [x] Implemented Serial Link Sync between instances.
- [x] Implemented Player 1 & Player 2 input mapping (Laptop Optimized).
- [ ] Implement Audio Routing (In Progress).
1. **Initialize Tauri Project**: Create a new Tauri project in a separate directory or within the `mgba-splitscreen` folder (TBD, probably separate and link to the fork as a submodule or external source).
2. **Setup Build Environment**: Ensure `cmake`, `clang`, and other dependencies are available to build `libmgba`.
3. **Rust Bindings**: Use `bindgen` to create Rust bindings for the `libmgba` headers in the fork.
4. **Implementation**: Follow the Phases in the PRD.

## Future Self: Notes
- The user specifically pointed to `Spuds0588/mgba-splitscreen.git`. Check if there are specific changes in that fork that differ from upstream mGBA which might be useful for the "splitscreen" goal.
- The PRD suggests a Tauri-based architecture where the frontend is just a canvas.
- Synchronization is key: "Perfect, drop-free synchronization between the two instances".
