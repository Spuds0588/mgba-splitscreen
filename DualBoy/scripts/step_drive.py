#!/usr/bin/env python3
"""Step driver: send inputs one at a time and dump a screenshot after each.

Used to map out GBA menu navigation (e.g. Four Swords boot -> multiplayer lobby)
by observing the screen after every input. Connect to a running app's WebSocket,
load a ROM, then run a list of inputs. After each input a PNG is saved.

Usage:
  step_drive.py <rom_path> --inputs "A WAIT:1500 DOWN A" --tag fs \
      [--boot 12] [--players 2] [--out /tmp]
"""
import argparse
import json
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


def save_png(rgba, path):
    img = Image.frombytes("RGBA", (GBA_W, GBA_H), rgba).convert("RGB")
    img.resize((GBA_W * 2, GBA_H * 2), Image.NEAREST).save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rom")
    ap.add_argument("--inputs", required=True, help="space-separated input list, e.g. 'A WAIT:1500 DOWN A' (P2: prefix per token)")
    ap.add_argument("--tag", default="step")
    ap.add_argument("--boot", type=float, default=10.0)
    ap.add_argument("--players", type=int, default=2)
    ap.add_argument("--out", default="/tmp")
    args = ap.parse_args()

    c = Client(args.players)
    c.load_rom(args.rom)
    print(f"boot wait {args.boot}s...")
    time.sleep(args.boot)

    step = 0
    for token in args.inputs.split():
        token = token.strip()
        if not token:
            continue
        player = 1
        tok, hold = token, None
        if ":" in token:
            head, h = token.split(":", 1)
            if head.startswith("P") and head[1:].isdigit():
                player = int(head[1:])
                tok, hold = (h.split(":", 1) + [None])[:2] if ":" in h else (h, None)
            else:
                tok, hold = head, h
        if tok == "WAIT":
            time.sleep((int(hold) if hold else 1000) / 1000.0)
            print(f"[{step:03d}] WAIT {hold}ms")
            step += 1
            continue
        if tok not in BUTTONS:
            print(f"skip unknown {tok!r}")
            continue
        c.tap(player, tok, hold_ms=int(hold) if hold else 120)
        print(f"[{step:03d}] P{player} {tok} (hold={hold or 120}ms)")
        step += 1
        for n in range(1, args.players + 1):
            f = c.player_frame(n)
            if f is not None:
                save_png(f, f"{args.out}/{args.tag}_s{step:03d}_p{n}.png")
        time.sleep(0.25)

    print("done")
    c.ws.close()


if __name__ == "__main__":
    main()
