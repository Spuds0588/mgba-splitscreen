# Test drivers

## ws_play.py — deterministic gameplay driver (preferred)

Drives the desktop app over its WebSocket (`ws://127.0.0.1:8088`): loads a ROM, injects
GBA button inputs per player, reads the real emulated frames back, and verifies the game
keeps animating. No display, no GTK dialog, fully deterministic.

```bash
cd DualBoy/src-tauri
cargo build
./target/debug/dualboy &
python3 ../scripts/ws_play.py "/path/to/rom.gba" \
  --boot 12 \
  --seq "A WAIT:2500 A A A A WAIT:600 START WAIT:600 A" \
  --dump /tmp/frame --watch 3
```

- Sequence tokens: `[P<N>:]BUTTON[:hold_ms]` (e.g. `P2:START:120`), plus `WAIT:ms`.
  Buttons: A B SELECT START RIGHT LEFT UP DOWN R L.
- `--dump /tmp/fs` saves `*_p1_boot.png`, `*_p2_boot.png`, and `*_pN_after.png` (2x
  scaled) so you can see the game state.
- Prints frame stats + a peak per-sample pixel delta: a large delta means the game is
  live-animating ("continues").
- Requires `websocket-client` and `PIL`: `pip install --target .freebuff/pylibs
  websocket-client pillow`.

Verified 2026-08-16 against all three `Test Roms/` ROMs (see `PROJECT_LOG.md`).

## gui_smoke.py — GUI smoke test (headless desktop verification)

`gui_smoke.py` drives the real DualBoy Tauri window over X11 to load a ROM through the
actual GTK file dialog — no code changes needed. It was used to verify the desktop app
end-to-end on a headless Linux box (XWayland under KDE, `DISPLAY=:1`).

Prefer `ws_play.py` when you need to actually play a game; use this to verify the real
UI (window, buttons, file dialog) still works.

## Prerequisites

- A running X display (e.g. XWayland session, or `Xvfb :1 &`).
- WebKitGTK/GTK3 runtime libs installed (Tauri's normal Linux deps).
- `python-xlib` (pure Python; install into a scratch dir, no root):
  `pip install --target .freebuff/pylibs python-xlib`

## Usage

```bash
cd DualBoy/src-tauri
export DISPLAY=:1 \
       WEBKIT_DISABLE_COMPOSITING_MODE=1 \
       WEBKIT_DISABLE_DMABUF_RENDERER=1 \
       LIBGL_ALWAYS_SOFTWARE=1
./target/debug/dualboy >/tmp/dualboy_app.log 2>&1 &
sleep 15   # let WebKit init
python3 /path/to/gui_smoke.py "/path/to/Test Roms/Legend of Zelda, The - A Link To The Past Four Swords (U) [!].gba"
```

The script: focuses the app window → End-scrolls the webview (wheel events don't reach
it) → finds and clicks the teal `Load ROM` button (located by grabbing the window and
searching for teal pixels) → in the GTK `Open File` dialog, pastes the ROM's directory
via an X11 CLIPBOARD selection owner + Ctrl+V → Enter → Escape → End x2 → Enter.

## Verifying the result

- `grep -c "GBA BIOS" /tmp/dualboy_app.log` should be in the hundreds (game booting).
- The app log should show lockstep sync: `Primary waiting for players to ack` /
  `All players acked, waking primary`.
- Live animation: capture the window twice, 2s apart
  (`xwd -id <win> a.xwd`, then `b.xwd`), convert with ffmpeg, diff with PIL — hundreds
  of thousands of differing pixels in the canvas area means it's rendering live.

## Gotchas baked into this approach

- This XWayland's core keyboard map has one keysym per keycode, so typing punctuation
  / shifted chars via XTEST is unreliable → the script pastes text through the
  clipboard instead.
- Window absolute origin must be `root.translate_coords(win, 0, 0)` (the reverse call
  returns the inverse).
- The GTK dialog is titled `Open File`; `xwd` captures it; use `ffmpeg` to convert
  xwd→png (PIL can't read this xwd variant).
- The file list is sorted by access time, newest first — the oldest ROM is the last
  row, which is why the script selects the last row.
