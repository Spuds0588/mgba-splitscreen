# GUI smoke test (headless desktop verification)

`gui_smoke.py` drives the real DualBoy Tauri window over X11 to load a ROM through the
actual GTK file dialog — no code changes needed. It was used to verify the desktop app
end-to-end on a headless Linux box (XWayland under KDE, `DISPLAY=:1`).

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
