#!/usr/bin/env python3
"""Robust frame capture for the DualBoy web server (port 8080).

Retries the connection and waits for a full 2-player video frame, then writes
per-player PNGs. Usage: python3 cap_frames.py [out_prefix] [players]
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from raw_ws import Client, GBA_W, GBA_H, FRAME_BYTES  # noqa: E402
from PIL import Image  # noqa: E402

PREFIX = sys.argv[1] if len(sys.argv) > 1 else "/tmp/cap"
PLAYERS = int(sys.argv[2]) if len(sys.argv) > 2 else 2


def try_once():
    c = Client(port=8080, path="/ws", players=PLAYERS)
    try:
        full = None
        deadline = time.time() + 8.0
        while time.time() < deadline:
            full = c.frame(2.0)
            if full is not None and len(full) >= PLAYERS * FRAME_BYTES + 1:
                break
            full = None
        if full is None:
            return None
        if full[0] != 0:
            # First byte should be the video tag; tolerate audio-tag lead-in by
            # waiting for a proper video frame.
            return None
        return full
    finally:
        c.close()


for attempt in range(8):
    full = try_once()
    if full is None:
        time.sleep(1.0)
        continue
    body = full[1:]
    ok = True
    for p in range(1, PLAYERS + 1):
        off = (p - 1) * FRAME_BYTES
        if off + FRAME_BYTES > len(body):
            ok = False
            break
        im = Image.frombytes("RGBA", (GBA_W, GBA_H), body[off:off + FRAME_BYTES]).convert("RGB")
        im.resize((GBA_W * 2, GBA_H * 2), Image.NEAREST).save(f"{PREFIX}_p{p}.png")
    if ok:
        print(f"captured {PLAYERS} players -> {PREFIX}_p*.png")
        sys.exit(0)
    time.sleep(1.0)
print("FAILED to capture frames after retries")
sys.exit(1)