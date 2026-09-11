#!/usr/bin/env python3
"""Deterministic step navigator for Four Swords (no screen classification).

Dumps a PNG after every button press so a human (or the next LLM session) can
verify each stage by eye. Use --players to limit, --steps to stop early.

  python3 step_nav2.py --port 8088 --path / --tag desk
  python3 step_nav2.py --port 8080 --path /ws --tag web

Flow per player (empirically mapped):
  boot -> A x2 (title -> CHOOSE A FILE -> ENTER A NAME)
       -> A x4 (type AAAA)
       -> DOWN x5, RIGHT x3, A (cursor walks 10-wide keyboard grid to END)
       -> "Now saving..." -> back at file screen
       -> A (select the save -> detail view) -> A (confirm)
       -> DOWN (Four Swords) -> A (select) -> connection/waiting
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


def save_frame(c, player, path):
    f = c.player_frame(player)
    if f is None:
        print(f"  (no frame for {path})")
        return
    Image.frombytes("RGBA", (GBA_W, GBA_H), f).convert("RGB").resize(
        (GBA_W * 2, GBA_H * 2), Image.NEAREST).save(path)


class Nav:
    def __init__(self, args, c):
        self.args = args
        self.c = c
        self.stage = 0

    def ok(self):
        return self.stage < self.args.steps

    def snap(self, player, name):
        save_frame(self.c, player, f"{self.args.dump}/{self.args.tag}_p{player}_{name}.png")

    def tap(self, player, button, wait, name, hold=200, gap=250):
        self.stage += 1
        if not self.ok():
            return False
        self.c.tap(player, button, hold_ms=hold, gap_ms=gap)
        time.sleep(wait)
        self.snap(player, name)
        return True

    def player(self, p):
        print(f"--- P{p}: boot -> name entry -> save ---")
        if not self.tap(p, "A", 1.2, "01_a1"): return
        if not self.tap(p, "A", 1.2, "02_a2"): return
        for i in range(4):
            if not self.tap(p, "A", 0.45, f"03_type{i+1}"): return
        for i in range(5):
            if not self.tap(p, "DOWN", 0.35, f"04_down{i+1}"): return
        for i in range(3):
            if not self.tap(p, "RIGHT", 0.35, f"05_right{i+1}"): return
        if not self.tap(p, "A", 8.5, "06_end_save"): return
        print(f"--- P{p} file chooser -> game ---")
        if not self.tap(p, "A", 1.2, "07_select_slot"): return
        if not self.tap(p, "A", 1.2, "08_confirm"): return
        if not self.tap(p, "DOWN", 0.8, "09_four_swords"): return
        if not self.tap(p, "A", 3.0, "10_connect"): return


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8088)
    ap.add_argument("--path", default="/")
    ap.add_argument("--tag", default="nav")
    ap.add_argument("--players", type=int, default=1)
    ap.add_argument("--steps", type=int, default=99)
    ap.add_argument("--watch", type=float, default=240.0)
    ap.add_argument("--dump", default="/tmp/stepnav")
    args = ap.parse_args()

    os.makedirs(args.dump, exist_ok=True)
    c = Client(port=args.port, path=args.path, players=args.players)
    if c.frame(10.0) is None:
        print("FAIL: no frames")
        return 1

    nav = Nav(args, c)
    time.sleep(3.0)
    nav.snap(1, "00_boot")
    for p in range(1, args.players + 1):
        nav.player(p)

    print("--- watching (dumps every 20s + final) ---")
    prev = {1: c.player_frame(1), 2: c.player_frame(2)}
    t = 0
    while t < args.watch and nav.ok():
        time.sleep(5.0)
        t += 5
        changed = []
        for p in (1, 2):
            f = c.player_frame(p)
            if f is None or prev[p] is None:
                continue
            n = sum(1 for i in range(0, min(len(f), len(prev[p])) - 4, 64)
                    if f[i] != prev[p][i] or f[i + 1] != prev[p][i + 1])
            if n:
                changed.append(f"P{p}:{n}px")
            prev[p] = f
            if t % 20 == 0:
                nav.snap(p, f"watch_{int(t)}")
        print(f"[watch {t}s] {' '.join(changed) if changed else 'both static'}")
        if args.players == 1:
            f = c.player_frame(2)
            changed2 = 0
            if f is not None and prev[2] is not None:
                changed2 = sum(1 for i in range(0, min(len(f), len(prev[2])) - 4, 64)
                               if f[i] != prev[2][i] or f[i + 1] != prev[2][i + 1])
                prev[2] = f
            if t % 20 == 0:
                nav.snap(2, f"watch2_{int(t)}")
            print(f"        (P2 idle; {changed2}px changed)")
    if args.players >= 2:
        nav.snap(1, "final")
        nav.snap(2, "final_p2")
    c.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())