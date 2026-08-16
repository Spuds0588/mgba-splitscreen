#!/usr/bin/env python3
"""Drive the running DualBoy Tauri app over its WebSocket (ws://127.0.0.1:8088)
to load a ROM, play into it with GBA button inputs, and verify the game keeps
animating.

Unlike gui_smoke.py / play_game.py (which drive the real GTK UI over X11), this
drives the emulator directly and reads the actual emulated frames back, so it is
deterministic and needs no display.

The desktop app's WebSocket accepts the same command protocol as the web demo:
  {"type":"load_rom","path":"..."}
  {"type":"keys","player":N,"keys":bits}

Sequence tokens are [P<N>:]BUTTON[:hold_ms]; WAIT:ms pauses. P<N> targets a
player (default 1). Example: "A WAIT:2500 P2:A P1:START".

Usage:
  ws_play.py <rom_path> [--players 2] [--boot 10] [--gap 300] \
      [--seq "A P2:START"] [--watch 3] [--dump /tmp/fs]
"""
import argparse
import json
import sys
import threading
import time

import websocket
from PIL import Image

GBA_W, GBA_H = 240, 160
FRAME_BYTES = GBA_W * GBA_H * 2  # RGB565, per player

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

    def _reader(self):
        while True:
            try:
                msg = self.ws.recv()
            except Exception:
                return
            if isinstance(msg, (bytes, bytearray)):
                with self.lock:
                    self.latest = bytes(msg)

    def start(self):
        threading.Thread(target=self._reader, daemon=True).start()

    def send(self, obj):
        self.ws.send(json.dumps(obj))

    def load_rom(self, path):
        print(f"load_rom {path}")
        self.send({"type": "load_rom", "path": path})

    def keys(self, player, bits):
        self.send({"type": "keys", "player": player, "keys": bits})

    def tap(self, player, button, hold_ms=90):
        self.keys(player, BUTTONS[button])
        time.sleep(hold_ms / 1000.0)
        self.keys(player, 0)

    def frame(self, timeout=5.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self.lock:
                if self.latest is not None:
                    return self.latest
            time.sleep(0.02)
        return None

    def player_frame(self, n, timeout=5.0):
        full = self.frame(timeout)
        if full is None:
            return None
        off = (n - 1) * FRAME_BYTES
        if off + FRAME_BYTES <= len(full):
            return full[off:off + FRAME_BYTES]
        return None


def frame_stats(p1):
    colors = set()
    nonblack = 0
    for i in range(0, len(p1), 2):
        c = (p1[i + 1] << 8) | p1[i]
        colors.add(c)
        if c != 0:
            nonblack += 1
    return len(colors), nonblack


def frame_diff(a, b):
    n = 0
    for i in range(0, min(len(a), len(b)), 2):
        if a[i] != b[i] or a[i + 1] != b[i + 1]:
            n += 1
    return n


def save_png(rgb565, path):
    img = Image.new("RGB", (GBA_W, GBA_H))
    px = img.load()
    for y in range(GBA_H):
        for x in range(GBA_W):
            i = (y * GBA_W + x) * 2
            c = (rgb565[i + 1] << 8) | rgb565[i]
            px[x, y] = (((c >> 11) & 0x1F) << 3, ((c >> 5) & 0x3F) << 2, (c & 0x1F) << 3)
    img.resize((GBA_W * 2, GBA_H * 2), Image.NEAREST).save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rom")
    ap.add_argument("--players", type=int, default=2)
    ap.add_argument("--boot", type=float, default=10.0)
    ap.add_argument("--gap", type=int, default=300)
    ap.add_argument("--seq", default="")
    ap.add_argument("--watch", type=float, default=3.0)
    ap.add_argument("--dump", default=None, help="save P1/P2 boot and post-sequence PNGs as <dump>_p1_boot.png etc.")
    args = ap.parse_args()

    c = Client(args.players)
    c.start()
    time.sleep(0.5)

    c.load_rom(args.rom)
    print(f"waiting {args.boot}s for boot...")
    time.sleep(args.boot)

    p1 = c.player_frame(1)
    if p1 is None:
        print("FAIL: no frame received")
        sys.exit(1)
    colors, nonblack = frame_stats(p1)
    print(f"booted: {colors} distinct colors, {nonblack}/{GBA_W*GBA_H} non-black pixels")

    if args.dump:
        save_png(p1, f"{args.dump}_p1_boot.png")
        for n in range(2, args.players + 1):
            f = c.player_frame(n)
            if f is not None:
                save_png(f, f"{args.dump}_p{n}_boot.png")

    # Send the input sequence: [P<n>:]BUTTON[:hold_ms] or WAIT:ms.
    for token in args.seq.split():
        token = token.strip()
        if not token:
            continue
        player = 1
        if ":" in token:
            head, h = token.split(":", 1)
            if head.startswith("P") and head[1:].isdigit():
                player = int(head[1:])
                tok = h
                hold = None
                if ":" in tok:
                    tok, hold = tok.split(":", 1)
            else:
                tok, hold = head, h
        else:
            tok, hold = token, None
        if tok == "WAIT":
            time.sleep((int(hold) if hold else 1000) / 1000.0)
            print(f"wait {hold}ms")
            continue
        if tok not in BUTTONS:
            print(f"ignoring unknown button {tok!r}")
            continue
        hms = int(hold) if hold else 90
        print(f"tap P{player} {tok}")
        c.tap(player, tok, hold_ms=hms)
        time.sleep(args.gap / 1000.0)

    if args.dump:
        for n in range(1, args.players + 1):
            f = c.player_frame(n)
            if f is not None:
                save_png(f, f"{args.dump}_p{n}_after.png")

    time.sleep(0.4)
    prev = c.player_frame(1)
    peak = 0
    samples = 0
    deadline = time.time() + args.watch
    while time.time() < deadline:
        cur = c.player_frame(1, timeout=1.0)
        if cur is not None and prev is not None:
            d = frame_diff(prev, cur)
            peak = max(peak, d)
            samples += 1
            prev = cur
        time.sleep(0.3)

    print(f"animation: {samples} samples, peak per-sample pixel delta = {peak}")
    if peak > 2000:
        print("RESULT: ANIMATING - game is running and continuing")
    else:
        print("RESULT: STATIC - game may be stalled or on a still frame")
    c.ws.close()


if __name__ == "__main__":
    main()
