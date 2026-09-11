#!/usr/bin/env python3
"""Adaptive Four Swords navigator.

Classifies the current screen (downscaled template match against reference frames
captured from this game) and navigates deterministically:

  boot -> CHOOSE A FILE -> (A on empty slot) -> ENTER A NAME (type AAAA, END)
       -> CHOOSE A FILE (save exists) -> (A on filled slot) -> CHOOSE A GAME
       -> (DOWN, A) Four Swords -> connection screen

Usage:
  adaptive_play.py <rom_path> [--players 2] [--refs DIR] [--dump DIR]
"""
import argparse
import json
import os
import sys
import threading
import time

import websocket
from PIL import Image

GBA_W, GBA_H = 240, 160
FRAME_BYTES = GBA_W * GBA_H * 4

BUTTONS = {
    "A": 1 << 0, "B": 1 << 1, "SELECT": 1 << 2, "START": 1 << 3,
    "RIGHT": 1 << 4, "LEFT": 1 << 5, "UP": 1 << 6, "DOWN": 1 << 7,
    "R": 1 << 8, "L": 1 << 9,
}

# Screen classifiers: label -> list of reference images (PIL, already downscaled).
REFS = {}


def load_refs(dirpath):
    global REFS
    for f in sorted(os.listdir(dirpath)):
        if not f.endswith(".png"):
            continue
        label, _, _ = f.partition("__")
        try:
            im = Image.open(os.path.join(dirpath, f)).convert("RGB")
        except Exception:
            continue
        im.thumbnail((48, 32))
        REFS.setdefault(label, []).append(im.convert("RGB"))
    print("loaded refs:", {k: len(v) for k, v in REFS.items()})


def classify(frame_rgba):
    """Return (label, score) for the best-matching reference screen."""
    im = Image.frombytes("RGBA", (GBA_W, GBA_H), frame_rgba).convert("RGB")
    im.thumbnail((48, 32))
    best_label, best_score = None, 1e18
    for label, refs in REFS.items():
        for ref in refs:
            if ref.size != im.size:
                continue
            s = 0
            for y in range(im.size[1]):
                for x in range(im.size[0]):
                    pr, pg, pb = im.getpixel((x, y))
                    rr, rg, rb = ref.getpixel((x, y))
                    s += abs(pr - rr) + abs(pg - rg) + abs(pb - rb)
            if s < best_score:
                best_score, best_label = s, label
    return best_label, best_score


class Client:
    def __init__(self, players):
        self.players = players
        self.latest = None
        self.lock = threading.Lock()
        self.ws = websocket.create_connection("ws://127.0.0.1:8088", timeout=5)
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        while True:
            try:
                msg = self.ws.recv()
            except Exception:
                return
            if isinstance(msg, (bytes, bytearray)):
                with self.lock:
                    self.latest = bytes(msg)

    def send(self, obj):
        self.ws.send(json.dumps(obj))

    def load_rom(self, path):
        self.send({"type": "load_rom", "path": path})

    def keys(self, player, bits):
        self.send({"type": "keys", "player": player, "keys": bits})

    def tap(self, player, button, hold_ms=120):
        self.keys(player, BUTTONS[button])
        time.sleep(hold_ms / 1000.0)
        self.keys(player, 0)

    def frame(self, timeout=3.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self.lock:
                if self.latest is not None:
                    return self.latest
            time.sleep(0.02)
        return None

    def player_frame(self, n, timeout=3.0):
        full = self.frame(timeout)
        if full is None:
            return None
        off = (n - 1) * FRAME_BYTES
        if off + FRAME_BYTES <= len(full):
            return full[off:off + FRAME_BYTES]
        return None


def wait_for(player_frame, want, timeout, poll=0.4):
    """Wait until the player's screen classifies as `want`."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        f = player_frame()
        if f is not None:
            label, _ = classify(f)
            if label == want:
                return True
        time.sleep(poll)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rom")
    ap.add_argument("--players", type=int, default=2)
    ap.add_argument("--refs", default="scripts/refs")
    ap.add_argument("--dump", default="/tmp/adaptive")
    ap.add_argument("--boot", type=float, default=15.0)
    args = ap.parse_args()

    load_refs(args.refs)
    c = Client(args.players)
    c.load_rom(args.rom)
    print(f"boot {args.boot}s...")
    time.sleep(args.boot)
    os.makedirs(args.dump, exist_ok=True)

    def snap(p, name):
        f = c.player_frame(p)
        if f is not None:
            im = Image.frombytes("RGBA", (GBA_W, GBA_H), f).convert("RGB")
            im.resize((GBA_W * 2, GBA_H * 2), Image.NEAREST).save(f"{args.dump}/{name}.png")

    # Per-player navigation state machine.
    # phase: "file" -> press A on empty slot; "name" -> type AAAA + END;
    #        "file2" -> press A on filled slot; "game" -> DOWN + A (Four Swords)
    def navigate(player):
        snap(player, f"p{player}_start")
        # 1. CHOOSE A FILE: press A until we leave the file screen (empty slot 1).
        for _ in range(4):
            label, _ = classify(c.player_frame(player))
            if label not in ("file", None):
                break
            c.tap(player, "A")
            time.sleep(1.5)
        time.sleep(1.0)
        # 2. ENTER A NAME: type AAAA then navigate to END (DOWN x5, RIGHT x3, A).
        snap(player, f"p{player}_name")
        for _ in range(4):
            c.tap(player, "A")
            time.sleep(0.4)
        for _ in range(5):
            c.tap(player, "DOWN")
            time.sleep(0.12)
        for _ in range(3):
            c.tap(player, "RIGHT")
            time.sleep(0.12)
        c.tap(player, "A")  # END -> Now saving...
        time.sleep(6.0)
        snap(player, f"p{player}_after_save")
        # 3. Back at CHOOSE A FILE (save on slot 1): press A on the filled slot.
        label, _ = classify(c.player_frame(player))
        if label in ("file", None):
            c.tap(player, "A")
            time.sleep(3.0)
        # 4. CHOOSE A GAME: DOWN to Four Swords, A to select.
        for _ in range(2):
            c.tap(player, "DOWN")
            time.sleep(0.3)
        c.tap(player, "A")
        time.sleep(4.0)
        snap(player, f"p{player}_final")

    for p in range(1, args.players + 1):
        print(f"--- navigating player {p} ---")
        navigate(p)

    print("done")
    c.ws.close()


if __name__ == "__main__":
    main()
