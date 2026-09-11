#!/usr/bin/env python3
"""Drive the DualBoy Tauri app over X11 to load a ROM through the real GTK file dialog.

Fully self-contained:
  1. Find the 'dualboy' window, raise + focus it
  2. Press End to scroll the webview to the bottom (deterministic)
  3. Grab the window pixels (XGetImage), locate the teal 'Load ROM' button
  4. Click it at correct ABSOLUTE screen coordinates
  5. In the 'Open File' dialog (keyboard-only, reliable keys only):
       a. Ctrl+L (location bar), paste the DIRECTORY path via an X11
          CLIPBOARD selection owner + Ctrl+V, Enter -> navigates to folder
       b. Escape to leave the location entry and focus the file list
       c. End to move the selection to the last row (the target ROM; the
          list sorts by access time, newest first, so the oldest is last)
       d. Enter to open the selected file
"""
import os, sys, threading, time
from PIL import Image
from Xlib import X, XK, display, protocol
from Xlib.ext import xtest

ROM_PATH = sys.argv[1] if len(sys.argv) > 1 else None

def log(msg):
    print(msg, flush=True)

d = display.Display(":1")
root = d.screen().root

CLIPBOARD = d.intern_atom("CLIPBOARD")
UTF8 = d.intern_atom("UTF8_STRING")
TEXT = d.intern_atom("TEXT")
STRING = d.intern_atom("STRING")
TARGETS = d.intern_atom("TARGETS")
ATOM_TYPE = d.intern_atom("ATOM")

def find_window(name_substr, depth=6):
    def walk(w, depth):
        if depth <= 0:
            return None
        try:
            wname = w.get_wm_name()
        except Exception:
            wname = None
        if wname and name_substr.lower() in wname.lower():
            return w
        try:
            for c in w.query_tree().children:
                r = walk(c, depth - 1)
                if r:
                    return r
        except Exception:
            pass
        return None
    return walk(root, depth)

def kc(keysym):
    return d.keysym_to_keycode(keysym)

def press_key(keycode):
    xtest.fake_input(d, X.KeyPress, keycode)
    xtest.fake_input(d, X.KeyRelease, keycode)
    d.sync()

def chord(mod_keycode, keycode):
    xtest.fake_input(d, X.KeyPress, mod_keycode)
    xtest.fake_input(d, X.KeyPress, keycode)
    xtest.fake_input(d, X.KeyRelease, keycode)
    xtest.fake_input(d, X.KeyRelease, mod_keycode)
    d.sync()

def click_abs(x, y):
    xtest.fake_input(d, X.MotionNotify, x=x, y=y, detail=0)
    d.sync()
    time.sleep(0.25)
    xtest.fake_input(d, X.ButtonPress, 1)
    d.sync()
    time.sleep(0.08)
    xtest.fake_input(d, X.ButtonRelease, 1)
    d.sync()
    log(f"clicked at abs ({x},{y})")

def grab_window_png(w):
    geom = w.get_geometry()
    raw = w.get_image(0, 0, geom.width, geom.height, X.ZPixmap, 0xFFFFFFFF)
    return Image.frombytes("RGB", (geom.width, geom.height), raw.data, "raw", "BGRX")

def find_load_rom_button(im):
    w, h = im.size
    def teal(p):
        return abs(p[0]-36) < 45 and abs(p[1]-200) < 45 and abs(p[2]-219) < 45
    rows = [sum(1 for x in range(0, w, 4) if teal(im.getpixel((x, y)))) for y in range(h)]
    bands, start = [], None
    for y, c in enumerate(rows):
        if c > 0 and start is None:
            start = y
        elif c == 0 and start is not None:
            bands.append((start, y - 1))
            start = None
    if start is not None:
        bands.append((start, h - 1))
    if not bands:
        return None
    a, b = bands[0]
    xs = [x for y in range(a, b, 2) for x in range(w) if teal(im.getpixel((x, y)))]
    return ((min(xs) + max(xs)) // 2, (a + b) // 2)

# ---- clipboard server ----
def serve_clipboard(text, timeout=8.0):
    win = root.create_window(0, 0, 1, 1, 0, X.CopyFromParent)
    win.set_selection_owner(CLIPBOARD, X.CurrentTime)
    d.sync()
    data = text.encode("utf-8")
    stop = time.time() + timeout

    def handle_selreq(req):
        target = req.target
        prop = req.property
        if target == TARGETS:
            win.change_property(prop, ATOM_TYPE, 32, [TARGETS, UTF8, TEXT, STRING])
        elif target in (UTF8, TEXT, STRING):
            win.change_property(prop, UTF8 if target == UTF8 else STRING, 8, data)
        else:
            return
        evt = protocol.event.SelectionNotify(
            time=X.CurrentTime, requestor=req.requestor, selection=req.selection,
            target=target, property=prop)
        req.requestor.send_event(evt)

    def loop():
        while time.time() < stop:
            try:
                ev = d.next_event()
            except Exception:
                break
            if ev.type == X.SelectionRequest:
                try:
                    handle_selreq(ev)
                except Exception as e:
                    log(f"clipboard serve error: {e}")
                d.sync()

    t = threading.Thread(target=loop, daemon=True)
    t.start()
    log(f"clipboard serving: {text!r}")
    return t

# ---- main flow ----
app = None
for _ in range(40):
    app = find_window("dualboy")
    if app:
        break
    time.sleep(0.5)
if not app:
    log("FAIL: no dualboy window")
    sys.exit(1)
geom = app.get_geometry()
trans = root.translate_coords(app, 0, 0)
abs_x, abs_y = trans.x, trans.y
log(f"window {hex(app.id)} {geom.width}x{geom.height} at abs ({abs_x},{abs_y})")

app.configure(stack_mode=X.Above)
try:
    app.set_input_focus(X.RevertToParent, X.CurrentTime)
except Exception as e:
    log(f"focus note: {e}")
d.sync()
time.sleep(1.2)

end_kc = kc(XK.string_to_keysym("End"))
press_key(end_kc)
time.sleep(0.5)
press_key(end_kc)
time.sleep(1.5)

im = grab_window_png(app)
pos = find_load_rom_button(im)
if not pos:
    log("FAIL: no teal button found in grab")
    sys.exit(3)
log(f"Load ROM button at rel ({pos[0]},{pos[1]})")

click_abs(abs_x + pos[0], abs_y + pos[1])
time.sleep(2.0)

dlg = None
for _ in range(30):
    dlg = find_window("Open File")
    if dlg:
        break
    time.sleep(0.5)
if not dlg:
    log("FAIL: GTK 'Open File' dialog did not appear")
    sys.exit(2)
log(f"dialog found: {hex(dlg.id)}")
try:
    dlg.configure(stack_mode=X.Above)
    dlg.set_input_focus(X.RevertToParent, X.CurrentTime)
except Exception as e:
    log(f"dialog focus note: {e}")
d.sync()
time.sleep(0.8)

# (a) navigate to the directory via Ctrl+L + paste + Enter
dirm = os.path.dirname(ROM_PATH)
chord(kc(XK.string_to_keysym("Control_L")), kc(XK.string_to_keysym("l")))
time.sleep(0.8)
serve_clipboard(dirm, timeout=6.0)
chord(kc(XK.string_to_keysym("Control_L")), kc(XK.string_to_keysym("v")))
time.sleep(0.8)
press_key(kc(XK.string_to_keysym("Return")))
d.sync()
log(f"pasted dir {dirm!r} + Enter")
time.sleep(3.0)

# (b) Escape -> focus the file list
esc_kc = kc(XK.string_to_keysym("Escape"))
press_key(esc_kc)
time.sleep(0.8)

# (c) End -> select the last row (the target ROM)
press_key(end_kc)
time.sleep(0.8)
press_key(end_kc)
time.sleep(0.8)

# (d) Enter -> open the selected file
press_key(kc(XK.string_to_keysym("Return")))
d.sync()
log("Escape, End x2, Enter sent")
time.sleep(2.0)

if find_window("Open File"):
    log("RESULT: dialog still open (ROM may not have loaded)")
else:
    log("RESULT: dialog closed (ROM accepted)")
sys.exit(0)
