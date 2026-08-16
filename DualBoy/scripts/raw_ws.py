#!/usr/bin/env python3
"""Minimal stdlib-only WebSocket client for driving the DualBoy Tauri app
(ws://127.0.0.1:8088). No external deps: raw socket + RFC6455 handshake.

Commands mirror the app's protocol:
  {"type":"load_rom","path":"..."}
  {"type":"keys","player":N,"keys":bits}

Usage:
  raw_ws.py load <rom_path>
  raw_ws.py keys <player> <keys>     # e.g. "1 8" = P1 START
  raw_ws.py frames <count>           # read N binary frames (default 3), print bytes
"""
import base64
import hashlib
import json
import os
import socket
import struct
import sys

HOST, PORT = "127.0.0.1", 8088
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def _read_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("socket closed")
        buf += chunk
    return buf


def connect():
    sock = socket.create_connection((HOST, PORT), timeout=10)
    key = base64.b64encode(os.urandom(16)).decode()
    req = (
        f"GET / HTTP/1.1\r\nHost: {HOST}:{PORT}\r\nUpgrade: websocket\r\n"
        f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
        f"Sec-WebSocket-Version: 13\r\n\r\n"
    )
    sock.sendall(req.encode())
    resp = b""
    while b"\r\n\r\n" not in resp:
        resp += sock.recv(4096)
    if b"101" not in resp.split(b"\r\n", 1)[0]:
        raise ConnectionError(f"handshake failed: {resp[:200]!r}")
    accept = hashlib.sha1((key + GUID).encode()).digest()
    expect = base64.b64encode(accept)
    if expect not in resp:
        raise ConnectionError(f"bad accept key: {resp[:200]!r}")
    return sock


def send_text(sock, obj):
    payload = json.dumps(obj).encode()
    mask = os.urandom(4)
    n = len(payload)
    if n < 126:
        header = bytes([0x81, 0x80 | n])
    elif n < 65536:
        header = bytes([0x81, 0x80 | 126]) + struct.pack(">H", n)
    else:
        header = bytes([0x81, 0x80 | 127]) + struct.pack(">Q", n)
    framed = header + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    sock.sendall(framed)


def recv_frame(sock, timeout=None):
    """Read one complete WS frame payload (handles 2-byte lengths; video frames
    are < 1 MB for 2-4 players so 16-bit length suffices)."""
    sock.settimeout(timeout)
    b0, b1 = _read_exact(sock, 2)
    length = b1 & 0x7F
    if length == 126:
        length = struct.unpack(">H", _read_exact(sock, 2))[0]
    elif length == 127:
        length = struct.unpack(">Q", _read_exact(sock, 8))[0]
    payload = _read_exact(sock, length)
    opcode = b0 & 0x0F
    return opcode, payload


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "frames"
    sock = connect()
    try:
        if cmd == "load":
            send_text(sock, {"type": "load_rom", "path": sys.argv[2]})
        elif cmd == "keys":
            send_text(sock, {"type": "keys", "player": int(sys.argv[2]), "keys": int(sys.argv[3])})
        count = int(sys.argv[2]) if cmd == "frames" else 3
        for _ in range(count):
            opcode, payload = recv_frame(sock, timeout=10)
            if opcode == 8:
                print("connection closed by server")
                return 1
            print(f"frame {opcode}: {len(payload)} bytes")
    finally:
        sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())