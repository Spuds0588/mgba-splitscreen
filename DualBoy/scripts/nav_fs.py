#!/usr/bin/env python3
"""Adaptive Four Swords navigator driven by pixel fingerprints (no fragile
template matching). Handles the run-to-run boot variance (title vs straight to
file select) by detecting the actual screen and pressing the right buttons.

Screens detected:
  title       - FS cart logo (big gold region)
  file        - CHOOSE A FILE (COPY/ERASE buttons at bottom)
  name        - ENTER A NAME keyboard (ABC/abc/backspace/END button row)
  choose      - CHOOSE A GAME (two panels)
  saving      - "Now saving..." (dark purple)
  multipak    - "turn power OFF and ON again" (failure screen)
  alttp       - A Link to the Past title (PRESS START)

Flow per player: boot -> (title/file) -> name -> type AAAA -> END -> save ->
file -> A (slot) -> A (confirm) -> choose -> RIGHT -> A (Four Swords) ->
connection screen.

Usage:
  python3 nav_fs.py --port 8088 --path / --tag desk --players 2 --watch 120
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from raw_ws import Client, GBA_W, GBA_H, FRAME_BYTES  # noqa: E402

from PIL import Image  # noqa: E402

BUTTON = {
    "A": 1 << 0, "B": 1 << 1, "SELECT": 1 << 2, "START": 1 << 3,
    "RIGHT": 1 << 4, "LEFT": 1 << 5, "UP": 1 << 6, "DOWN": 1 << 7,
}


def rgb(frame):
    return Image.frombytes("RGBA", (GBA_W, GBA_H), frame).convert("RGB")


def detect(frame):
    """Return the best-guess screen label for an RGBA frame."""
    im = rgb(frame)
    px = im.load()

    def frac(x0, y0, x1, y1, pred):
        n = tot = 0
        for y in range(y0, y1, 2):
            for x in range(x0, x1, 2):
                r, g, b = px[x, y]
                tot += 1
                if pred(r, g, b):
                    n += 1
        return n / tot if tot else 0

    gold = lambda r, g, b: r > 170 and g > 110 and b < 120          # noqa: E731
    dark = lambda r, g, b: r < 60 and g < 60 and b < 90             # noqa: E731
    purple = lambda r, g, b: r > 90 and b > 110 and r > g + 40      # noqa: E731
    light = lambda r, g, b: r > 150 and g > 150 and b > 170         # noqa: E731  (keyboard letters)
    green = lambda r, g, b: g > 100 and g > r + 30 and g > b + 20   # noqa: E731

    gold_top = frac(40, 20, 200, 95, gold)      # FS cart logo (red/gold ZELDA)
    dark_mid = frac(10, 40, 230, 120, dark)
    purple_mid = frac(10, 40, 230, 130, purple)
    green_mid = frac(10, 60, 230, 120, green)

    # Name keyboard: the letter grid spreads light pixels across MANY rows (a
    # dialog box's text occupies only one band), so count distinct light rows.
    light_rows = 0
    light_total = 0
    for y in range(70, 135, 2):
        row = 0
        for x in range(20, 220, 2):
            r, g, b = px[x, y]
            if r > 150 and g > 150 and b > 170:
                row += 1
        light_total += row
        if row >= 2:
            light_rows += 1

    # Multipak warning: mostly dark screen with a small centered box.
    if dark_mid > 0.5:
        return "multipak"
    # Saving dialog: purple-ish dark screen.
    if purple_mid > 0.25:
        return "saving"
    # FS cart title: gold/red logo in the upper half.
    if gold_top > 0.08:
        return "title"
    # ALttP title: bright sky/landscape (light across the whole mid region).
    if green_mid > 0.25 or light_total > 400:
        return "alttp"
    # Name keyboard: a letter grid spanning many rows with enough total light
    # (a dialog box's 2 lines give ~9 rows but only ~85 total; the keyboard ~137).
    if light_rows >= 8 and light_total > 100:
        return "name"
    # CHOOSE A GAME: green landscape with the two panels.
    if green_mid > 0.10:
        return "choose"
    # File select: blue slots + COPY/ERASE (no light grid, no gold, no green).
    return "file"


def cursor_pos(frame):
    """Pixel-detected keyboard cursor (cx, cy) or None."""
    im = rgb(frame)
    xs, ys = [], []
    for y in range(66, 150, 2):
        for x in range(0, 240, 2):
            r, g, b = im.getpixel((x, y))
            if r > 170 and 60 < g < 170 and 60 < b < 170 and r > g + 30:
                xs.append(x)
                ys.append(y)
    if not xs:
        return None
    return sum(xs) // len(xs), sum(ys) // len(ys)


def save_frame(c, player, path):
    f = c.player_frame(player)
    if f is None:
        return
    rgb(f).resize((GBA_W * 2, GBA_H * 2), Image.NEAREST).save(path)


class Nav:
    def __init__(self, args, c):
        self.args = args
        self.c = c
        self.stage = 0

    def snap(self, player, name):
        save_frame(self.c, player, f"{self.args.dump}/{self.args.tag}_p{player}_{name}.png")

    def tap(self, player, button, name, wait=0.4, hold=200, gap=250):
        self.c.tap(player, button, hold_ms=hold, gap_ms=gap)
        time.sleep(wait)
        self.snap(player, name)

    def wait_screen(self, player, want, timeout, name, poll=0.5):
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            f = self.c.player_frame(player, timeout=2.0)
            if f is not None:
                last = detect(f)
                if last in want:
                    return last
            time.sleep(poll)
        self.snap(player, f"stuck_{name}_{last}")
        return last

    def to_name_entry(self, p):
        """Boot -> ENTER A NAME, whatever the boot screen was."""
        for _ in range(12):
            f = self.c.player_frame(p, timeout=2.0)
            if f is None:
                time.sleep(1.0)
                continue
            s = detect(f)
            if s == "name":
                return True
            if s == "title":
                self.tap(p, "A", "title_a", wait=1.0)
            elif s == "file":
                self.tap(p, "A", "file_a", wait=1.0)
            elif s in ("choose", "unknown"):
                # Unknown/choose: A is usually safe at boot.
                self.tap(p, "A", "unknown_a", wait=1.0)
            time.sleep(0.4)
        return False

    def reset_cursor_to_a(self, p):
        """Walk the keyboard cursor up/left to the top-left (A) using pixel
        detection, so the END walk below is deterministic."""
        for _ in range(8):
            pos = cursor_pos(self.c.player_frame(p))
            if pos is None:
                time.sleep(0.3)
                continue
            cx, cy = pos
            if cy > 72:  # row 1 cursor sits around y~68; rows below are higher y
                self.tap(p, "UP", "up", wait=0.25)
            elif cx > 34:  # col 1 cursor sits around x~30
                self.tap(p, "LEFT", "left", wait=0.25)
            else:
                return True
        return True

    def player(self, p):
        print(f"--- P{p}: to name entry ---")
        if not self.to_name_entry(p):
            print(f"  P{p}: never reached name entry")
            return
        self.snap(p, "name_entry")
        self.reset_cursor_to_a(p)
        self.snap(p, "cursor_a")
        # Type AAAA (cursor stays on A while typing).
        for _ in range(4):
            self.tap(p, "A", "type", wait=0.4)
        self.snap(p, "typed")
        # Walk to END: DOWN x5, RIGHT x3, A.
        for _ in range(5):
            self.tap(p, "DOWN", "down", wait=0.3)
        for _ in range(3):
            self.tap(p, "RIGHT", "right", wait=0.3)
        self.tap(p, "A", "end", wait=1.0)
        # Wait for save to finish -> back at file select.
        s = self.wait_screen(p, {"file", "choose", "saving"}, 20, "after_save")
        print(f"  P{p}: after save -> {s}")
        self.snap(p, "after_save")
        # Select the filled slot, then confirm into CHOOSE A GAME.
        self.tap(p, "A", "slot", wait=1.2)
        g = self.wait_screen(p, {"choose", "file"}, 10, "choose")
        if g == "file":
            self.tap(p, "A", "slot2", wait=1.2)
            g = self.wait_screen(p, {"choose"}, 8, "choose2")
        print(f"  P{p}: game select -> {g}")
        self.snap(p, "choose_game")
        # Four Swords is the RIGHT panel: RIGHT then A.
        self.tap(p, "RIGHT", "right_fs", wait=0.8)
        self.tap(p, "A", "select_fs", wait=3.0)
        self.snap(p, "after_select")
        print(f"  P{p}: selected, waiting for link...")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8088)
    ap.add_argument("--path", default="/")
    ap.add_argument("--tag", default="nav")
    ap.add_argument("--players", type=int, default=2)
    ap.add_argument("--watch", type=float, default=120.0)
    ap.add_argument("--dump", default="/tmp/navfs")
    ap.add_argument("--rom", default=None)
    args = ap.parse_args()

    os.makedirs(args.dump, exist_ok=True)
    if args.rom:
        if args.port == 8080:
            import urllib.request
            with open(args.rom, "rb") as fh:
                urllib.request.urlopen(urllib.request.Request(
                    "http://127.0.0.1:8080/load_rom", data=fh.read(), method="POST"), timeout=30)
            print("ROM POSTed to web server")
        else:
            c0 = Client(port=args.port, path=args.path, players=args.players)
            c0.load_rom(args.rom)
            c0.close()
            print("ROM loaded via desktop WS")
        time.sleep(6.0)

    c = Client(port=args.port, path=args.path, players=args.players)
    if c.frame(12.0) is None:
        print("FAIL: no frames")
        return 1
    nav = Nav(args, c)
    time.sleep(2.0)
    nav.snap(1, "boot")
    for p in range(1, args.players + 1):
        nav.player(p)
        time.sleep(2.0)

    print("--- watching (detect both every 5s) ---")
    last = {}
    t = 0
    while t < args.watch:
        time.sleep(5.0)
        t += 5
        for p in range(1, args.players + 1):
            f = c.player_frame(p, timeout=2.0)
            if f is None:
                continue
            s = detect(f)
            if last.get(p) != s:
                last[p] = s
                print(f"  [{t}s] P{p}: {s}")
                nav.snap(p, f"watch_{int(t)}")
    print("final screens:", {p: last.get(p) for p in range(1, args.players + 1)})
    for p in range(1, args.players + 1):
        nav.snap(p, "final")
    c.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())