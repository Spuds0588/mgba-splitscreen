#!/usr/bin/env python3
"""Drive BOTH players in lockstep to the Four Swords linking screen, then press
START on both simultaneously and watch the handshake.

Assumes the FS cart is at (or can reach) the file-select screen, i.e. a save slot
already exists (created by nav_fs.py on a prior run). Sends identical, synchronized
inputs to P1 and P2 so the two games enter the link phase together.

Usage:
  python3 drive_fs_link.py [--players 2] [--watch 90] [--dump /tmp/fslink]
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from raw_ws import Client, GBA_W, GBA_H  # noqa: E402
from PIL import Image  # noqa: E402

BUTTON = {
    "A": 1 << 0, "B": 1 << 1, "SELECT": 1 << 2, "START": 1 << 3,
    "RIGHT": 1 << 4, "LEFT": 1 << 5, "UP": 1 << 6, "DOWN": 1 << 7,
}


def rgb(frame):
    return Image.frombytes("RGBA", (GBA_W, GBA_H), frame).convert("RGB")


def detect(frame):
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

    gold = lambda r, g, b: r > 170 and g > 110 and b < 120
    dark = lambda r, g, b: r < 60 and g < 60 and b < 90
    purple = lambda r, g, b: r > 90 and b > 110 and r > g + 40
    light = lambda r, g, b: r > 150 and g > 150 and b > 170
    green = lambda r, g, b: g > 100 and g > r + 30 and g > b + 20

    gold_top = frac(40, 20, 200, 95, gold)
    dark_mid = frac(10, 40, 230, 120, dark)
    purple_mid = frac(10, 40, 230, 130, purple)
    green_mid = frac(10, 60, 230, 120, green)

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

    if dark_mid > 0.5:
        return "multipak"
    if purple_mid > 0.25:
        return "saving"
    if gold_top > 0.08:
        return "title"
    if green_mid > 0.25 or light_total > 400:
        return "alttp"
    if light_rows >= 8 and light_total > 100:
        return "name"
    if green_mid > 0.10:
        return "choose"
    return "file"


class Driver:
    def __init__(self, args, c):
        self.args = args
        self.c = c

    def snap(self, name):
        for p in range(1, self.args.players + 1):
            f = self.c.player_frame(p, timeout=3.0)
            if f is not None:
                rgb(f).resize((GBA_W * 2, GBA_H * 2), Image.NEAREST).save(
                    f"{self.args.dump}/{self.args.tag}_p{p}_{name}.png")

    def both(self, button, hold=200, wait=0.5):
        """Tap the same button on all players at the same time."""
        for p in range(1, self.args.players + 1):
            self.c.keys(p, BUTTON[button])
        time.sleep(hold / 1000.0)
        for p in range(1, self.args.players + 1):
            self.c.keys(p, 0)
        time.sleep(wait)

    def states(self):
        out = []
        for p in range(1, self.args.players + 1):
            f = self.c.player_frame(p, timeout=3.0)
            out.append(detect(f) if f is not None else "NOFRAME")
        return out

    def cursor_pos(self, frame):
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

    def reset_cursor_to_a(self):
        for _ in range(10):
            poss = []
            for p in range(1, self.args.players + 1):
                f = self.c.player_frame(p, timeout=3.0)
                poss.append(self.cursor_pos(f) if f is not None else None)
            if all(pos is not None for pos in poss):
                done = all(cy <= 72 and cx <= 34 for cx, cy in poss)
                if done:
                    return True
                any_down = any(cy > 72 for _, cy in poss)
                any_right = any(cx > 34 for cx, _ in poss)
                if any_down:
                    self.both("UP", wait=0.25)
                elif any_right:
                    self.both("LEFT", wait=0.25)
            else:
                time.sleep(0.3)
        return True

    def do_name_entry(self):
        """Both players: cursor to A, type AAAA, walk to END, confirm."""
        self.reset_cursor_to_a()
        for _ in range(4):
            self.both("A", wait=0.5)
        for _ in range(5):
            self.both("DOWN", wait=0.3)
        for _ in range(3):
            self.both("RIGHT", wait=0.3)
        self.both("A", wait=1.2)

    def drive_to_fs_title(self):
        """Full boot flow to FS title, driving both players together."""
        for attempt in range(40):
            s = self.states()
            print(f"  [{attempt}] states={s}", flush=True)
            if all(x == "title" for x in s):
                return True
            if all(x == "name" for x in s):
                self.do_name_entry()
                continue
            if all(x == "saving" for x in s):
                self.both("A", wait=1.5)
                continue
            if all(x == "file" for x in s):
                # Open the (now-filled) slot, then confirm into CHOOSE A GAME.
                self.both("A", wait=1.5)
                continue
            if all(x == "choose" for x in s):
                self.both("RIGHT", wait=0.7)
                self.both("A", wait=3.0)
                continue
            if all(x == "alttp" for x in s):
                # Landed on ALttP panel by mistake: back out and reselect FS.
                self.both("B", wait=1.0)
                continue
            # Mixed/unknown: gentle A to nudge both forward.
            self.both("A", wait=1.5)
        return False

    def run(self):
        if not self.drive_to_fs_title():
            print("FAIL: could not reach FS title on both", flush=True)
            return 1
        self.snap("fs_title")
        print("Both at FS title; pressing START simultaneously...", flush=True)
        self.both("START", hold=200, wait=2.0)
        self.snap("started")

        # Watch: report states + how much SIO activity is happening.
        last = None
        t = 0
        while t < self.args.watch:
            time.sleep(5.0)
            t += 5
            s = self.states()
            if s != last:
                last = s
                print(f"  [{t}s] states={s}", flush=True)
                self.snap(f"watch_{int(t)}")
        self.snap("final")
        print("final states:", self.states(), flush=True)
        return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--path", default="/ws")
    ap.add_argument("--players", type=int, default=2)
    ap.add_argument("--tag", default="fslink")
    ap.add_argument("--watch", type=float, default=90.0)
    ap.add_argument("--dump", default="/tmp/fslink")
    args = ap.parse_args()
    os.makedirs(args.dump, exist_ok=True)

    c = Client(port=args.port, path=args.path, players=args.players)
    if c.frame(10.0) is None:
        print("FAIL: no frames", flush=True)
        return 1
    d = Driver(args, c)
    rc = d.run()
    c.close()
    return rc


if __name__ == "__main__":
    sys.exit(main())
