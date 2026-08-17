#!/usr/bin/env python3
"""Minimal stdlib-only WebSocket client + frame reader for DualBoy.

No external dependencies (raw socket + RFC6455 handshake). Speaks the app's
command protocol on both the desktop app (ws://127.0.0.1:8088, path "/") and
the web demo (ws://127.0.0.1:8080/ws):

  {"type":"load_rom","path":"..."}
  {"type":"keys","player":N,"keys":bits}

CLI usage:
  raw_ws.py load <rom_path> [--port 8088]
  raw_ws.py keys <player> <keys> [--port 8080] [--path /ws]
  raw_ws.py frames <count> [--port 8088]
  raw_ws.py fps <seconds> [--port 8080] [--path /ws]
  raw_ws.py tap <player> <BUTTON> [--port 8088]

As a library (dependency-free; PIL optional for pixels):
  from raw_ws import Client
  c = Client(port=8088, path="/")
  c.load_rom("/path/to.gba")
  c.tap(1, "START")
  full = c.frame()                # latest broadcast frame (all players, RGBA)
  c.fps(5.0)                      # measure delivered frame rate
"""
import argparse
import base64
import hashlib
import json
import os
import socket
import struct
import sys
import threading
import time

DEFAULT_HOST, DEFAULT_PORT, DEFAULT_PATH = "127.0.0.1", 8088, "/"
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

GBA_W, GBA_H = 240, 160
FRAME_BYTES = GBA_W * GBA_H * 4  # RGBA8888 per player

BUTTONS = {
    "A": 1 << 0, "B": 1 << 1, "SELECT": 1 << 2, "START": 1 << 3,
    "RIGHT": 1 << 4, "LEFT": 1 << 5, "UP": 1 << 6, "DOWN": 1 << 7,
    "R": 1 << 8, "L": 1 << 9,
}


def _read_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("socket closed")
        buf += chunk
    return buf


def _handshake(sock, host, port, path):
    key = base64.b64encode(os.urandom(16)).decode()
    req = (
        f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\n"
        f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
        f"Sec-WebSocket-Version: 13\r\n\r\n"
    )
    sock.sendall(req.encode())
    resp = b""
    while b"\r\n\r\n" not in resp:
        resp += sock.recv(4096)
    if b"101" not in resp.split(b"\r\n", 1)[0]:
        raise ConnectionError(f"handshake failed: {resp[:200]!r}")
    accept = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest())
    if accept not in resp:
        raise ConnectionError(f"bad accept key: {resp[:200]!r}")


def recv_frame(sock, timeout=None):
    """Read one complete WS frame, returning (opcode, payload)."""
    sock.settimeout(timeout)
    b0, b1 = _read_exact(sock, 2)
    length = b1 & 0x7F
    if length == 126:
        length = struct.unpack(">H", _read_exact(sock, 2))[0]
    elif length == 127:
        length = struct.unpack(">Q", _read_exact(sock, 8))[0]
    payload = _read_exact(sock, length)
    return b0 & 0x0F, payload


def send_text(sock, payload):
    """Send a masked text (opcode 0x81) frame — the app only parses text JSON."""
    mask = os.urandom(4)
    n = len(payload)
    if n < 126:
        header = bytes([0x81, 0x80 | n])
    elif n < 65536:
        header = bytes([0x81, 0x80 | 126]) + struct.pack(">H", n)
    else:
        header = bytes([0x81, 0x80 | 127]) + struct.pack(">Q", n)
    sock.sendall(header + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))


class Client:
    """Connects to a DualBoy frame/command socket and keeps the latest frame."""

    def __init__(self, host=DEFAULT_HOST, port=DEFAULT_PORT, path=DEFAULT_PATH,
                 players=2, timeout=10):
        self.players = players
        self.latest = None
        self.lock = threading.Lock()
        self.sock = socket.create_connection((host, port), timeout=timeout)
        _handshake(self.sock, host, port, path)
        self._stop = False
        self._thread = threading.Thread(target=self._reader, daemon=True)
        self._thread.start()

    def _reader(self):
        while not self._stop:
            try:
                op, payload = recv_frame(self.sock)
            except Exception:
                break
            if op == 8:  # close
                break
            if op == 2:  # binary frame = pixels
                with self.lock:
                    self.latest = payload

    def send(self, obj):
        send_text(self.sock, json.dumps(obj).encode())

    def load_rom(self, path):
        self.send({"type": "load_rom", "path": path})

    def keys(self, player, bits):
        self.send({"type": "keys", "player": player, "keys": bits})

    def tap(self, player, button, hold_ms=120, gap_ms=0):
        self.keys(player, BUTTONS[button])
        time.sleep(hold_ms / 1000.0)
        self.keys(player, 0)
        if gap_ms:
            time.sleep(gap_ms / 1000.0)

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

    def fps(self, seconds=5.0):
        """Count broadcast frames over `seconds`; returns (fps, n_frames)."""
        time.sleep(0.3)  # let the socket drain before counting
        started = time.time()
        n = 0
        deadline = started + seconds
        while time.time() < deadline:
            with self.lock:
                if self.latest is not None:
                    n += 1
                    self.latest = None
            time.sleep(0.005)
        return n / seconds, n

    def close(self):
        self._stop = True
        try:
            self.sock.close()
        except Exception:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["load", "keys", "frames", "fps", "tap"])
    ap.add_argument("args", nargs="*")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--path", default=DEFAULT_PATH)
    args = ap.parse_args()

    c = Client(port=args.port, path=args.path, players=2)
    try:
        if args.cmd == "load":
            c.load_rom(args.args[0])
        elif args.cmd == "keys":
            c.keys(int(args.args[0]), int(args.args[1]))
        elif args.cmd == "tap":
            c.tap(int(args.args[0]), args.args[1], hold_ms=120)
        elif args.cmd == "frames":
            for _ in range(int(args.args[0])):
                op, payload = recv_frame(c.sock, timeout=10)
                print(f"frame {op}: {len(payload)} bytes")
        elif args.cmd == "fps":
            rate, n = c.fps(float(args.args[0]) if args.args else 5.0)
            print(f"fps: {rate:.1f} ({n} frames)")
        time.sleep(0.5)
    finally:
        c.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())