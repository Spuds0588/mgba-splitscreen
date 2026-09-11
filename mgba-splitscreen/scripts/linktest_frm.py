#!/usr/bin/env python3
"""Read the linktest ROM's on-screen FRM counter for each player and check parity.

The linktest draws FRM (game frames) right-aligned as 8 decimal digits in cyan at
x=36..84, y=37..44 (6px per digit, 5x7 glyphs). This decodes the cyan pixels
against the ROM's own font and compares the four players' counters.

Usage: python3 linktest_frm.py <players> [label]
"""
import sys
import time

sys.path.insert(0, "DualBoy/scripts")
from raw_ws import Client, GBA_W, GBA_H

# linktest font (digits 0-9), 7 rows x 5 cols, '1' = lit
DIGITS = {
    "0": "01110" "10001" "10011" "10101" "11001" "10001" "01110",
    "1": "00100" "01100" "00100" "00100" "00100" "00100" "01110",
    "2": "01110" "10001" "00001" "00010" "00100" "01000" "11111",
    "3": "11111" "00010" "00100" "00010" "00001" "10001" "01110",
    "4": "00010" "00110" "01010" "10010" "11111" "00010" "00010",
    "5": "11111" "10000" "11110" "00001" "00001" "10001" "01110",
    "6": "00110" "01000" "10000" "11110" "10001" "10001" "01110",
    "7": "11111" "00001" "00010" "00100" "01000" "01000" "01000",
    "8": "01110" "10001" "10001" "01110" "10001" "10001" "01110",
    "9": "01110" "10001" "10001" "01111" "00001" "00010" "01100",
}


def read_frm(frame):
    """Decode the FRM counter from an RGBA frame. Returns int or None."""
    # cyan text: sample the first lit pixel to learn its exact RGBA color.
    lit = None
    for y in range(37, 44):
        for x in range(36, 84):
            off = (y * GBA_W + x) * 4
            r, g, b = frame[off], frame[off + 1], frame[off + 2]
            if r < 60 and g > 200 and b > 200:  # cyan (0,255,255)
                lit = (r, g, b)
                break
        if lit:
            break
    if lit is None:
        return None

    digits = []
    # 8 digits, right-aligned: iterate left-to-right over 6px cells.
    for d in range(8):
        x0 = 36 + d * 6
        bits = []
        for y in range(7):
            row = 0
            for x in range(5):
                off = ((37 + y) * GBA_W + (x0 + x)) * 4
                r, g, b = frame[off], frame[off + 1], frame[off + 2]
                if (r, g, b) == lit:
                    row |= 1 << (4 - x)
            bits.append(f"{row:05b}")
        pattern = "".join(bits)
        # match against known digits
        best = None
        for ch, glyph in DIGITS.items():
            if glyph == pattern:
                best = ch
                break
        # blank cell -> space (leading zero padding)
        if best is None and set(pattern) <= {"0"}:
            best = " "
        digits.append(best if best else "?")
    s = "".join(digits).strip()
    return int(s) if s.isdigit() else None


def main():
    players = int(sys.argv[1]) if len(sys.argv) > 1 else 2
    label = sys.argv[2] if len(sys.argv) > 2 else "s1"
    c = Client(port=8080, path="/ws", players=players)

    def sample():
        out = {}
        for p in range(1, players + 1):
            f = c.player_frame(p, timeout=3.0)
            out[p] = read_frm(f) if f is not None else None
        return out

    s1 = sample()
    print(f"[{label}] t0: {s1}", flush=True)
    time.sleep(10)
    s2 = sample()
    print(f"[{label}] t1: {s2}", flush=True)
    deltas = {p: (s2[p] - s1[p]) if (s1[p] is not None and s2[p] is not None) else None for p in s1}
    print(f"[{label}] deltas over 10s: {deltas}", flush=True)
    vals = [d for d in deltas.values() if d is not None]
    if vals and max(vals) - min(vals) <= 1:
        print(f"[{label}] PARITY OK (all within 1 frame)", flush=True)
    else:
        print(f"[{label}] PARITY MISMATCH", flush=True)
    c.close()


if __name__ == "__main__":
    main()
