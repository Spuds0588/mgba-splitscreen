#!/usr/bin/env python3
"""Full DualBoy multiplayer test: drives BOTH apps into a real Four Swords
session and compares their delivered frame rates.

Dependency-free (raw_ws.py + PIL, both already in this repo's scripts).

What it does per target (desktop 8088 "/" or web 8080 "/ws"):
  1. Load Four Swords, wait for boot, measure idle FPS.
  2. Navigate BOTH players through: CHOOSE A FILE -> ENTER A NAME (AAAA + END)
     -> save -> back at FILE -> A on filled slot -> CHOOSE A GAME -> DOWN+A
     (Four Swords) -> connection screen. Every transition is screenshot-dumped
     to --dump/<tag>_p<N>_<step>.png for diagnosis.
  3. Watch for up to --post seconds classifying both screens; reports whether
     they stay in menus, reach the multi-pak warning (failure), or leave the
     menus entirely (likely in a session).

Usage:
  link_test.py --port 8088 --path / --tag desktop [--rom <path>] [--dump /tmp/linktest]
  link_test.py --port 8080 --path /ws --tag web     [--rom <path>]
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
    "R": 1 << 8, "L": 1 << 9,
}

REFS = {}


def load_refs(dirpath):
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
    print(f"refs: {{{', '.join(f'{k}:{len(v)}' for k, v in REFS.items())}}}")


def classify(rgba):
    """Return (label, score) of best-matching reference screen."""
    im = Image.frombytes("RGBA", (GBA_W, GBA_H), rgba).convert("RGB")
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


def save(rgba, path):
    im = Image.frombytes("RGBA", (GBA_W, GBA_H), rgba).convert("RGB")
    im.resize((GBA_W * 2, GBA_H * 2), Image.NEAREST).save(path)


def wait(c, player, want_set, timeout, poll=0.5, dump=None, step=""):
    """Wait until player's screen classifies into want_set. Returns label."""
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        f = c.player_frame(player, timeout=2.0)
        if f is not None:
            label, _ = classify(f)
            last = label
            if label in want_set:
                return label
            if dump and time.time() % 3 < 0.1:
                save(f, f"{dump}_p{player}_{step}_misc_{label}.png")
        time.sleep(poll)
    return last


def navigate(c, player, dump, tag):
    """Drive one player to the Four Swords connection screen."""
    def snap(name):
        f = c.player_frame(player)
        if f is not None:
            save(f, f"{dump}/{tag}_p{player}_{name}.png")

    # 1. Boot: title or CHOOSE A FILE -> press A until the name-entry opens.
    label = wait(c, player, {"file", "name"}, 25, step="boot")
    print(f"  p{player} boot screen: {label}")
    for _ in range(6):
        f = c.player_frame(player)
        if f is None:
            return False
        lbl, _ = classify(f)
        if lbl == "name":
            break
        if lbl not in ("file", "alttp", None):
            break
        c.tap(player, "A", hold_ms=200, gap_ms=900)
    time.sleep(1.0)

    # 2. Type AAAA (A x4).
    for _ in range(4):
        c.tap(player, "A", hold_ms=140, gap_ms=330)
    snap("typed")

    # 3. Keyboard: DOWN x5, RIGHT x3 -> END -> save.
    for _ in range(5):
        c.tap(player, "DOWN", hold_ms=100, gap_ms=300)
    for _ in range(3):
        c.tap(player, "RIGHT", hold_ms=100, gap_ms=300)
    c.tap(player, "A", hold_ms=150, gap_ms=400)  # END -> Now saving...
    snap("end")
    wait(c, player, {"saving"}, 8, step="saving")
    snap("saving")

    # 4. Save completes: back at CHOOSE A FILE (with filled slot).
    f = wait(c, player, {"file"}, 25, step="after_save")
    print(f"  p{player} after save: {f}")
    time.sleep(1.0)
    snap("file2")

    # 5. A on the filled slot -> CHOOSE A GAME.
    c.tap(player, "A", hold_ms=200, gap_ms=500)
    g = wait(c, player, {"game"}, 12, step="game_screen")
    print(f"  p{player} game screen: {g}")
    snap("game")
    time.sleep(1.0)

    # 6. DOWN to Four Swords, A to select -> connection/waiting screen.
    c.tap(player, "DOWN", hold_ms=150, gap_ms=350)
    c.tap(player, "A", hold_ms=200, gap_ms=400)
    time.sleep(3.0)
    snap("connect")
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8088)
    ap.add_argument("--path", default="/")
    ap.add_argument("--tag", default="desktop")
    ap.add_argument("--rom", default=None)
    ap.add_argument("--players", type=int, default=2)
    ap.add_argument("--refs", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "refs"))
    ap.add_argument("--dump", default="/tmp/linktest")
    ap.add_argument("--watch", type=float, default=45.0)  # seconds after both connect
    ap.add_argument("--no-load", action="store_true")
    args = ap.parse_args()

    load_refs(args.refs)
    os.makedirs(args.dump, exist_ok=True)

    if args.rom and not args.no_load:
        # Web demo loads over HTTP POST; desktop over its WS protocol.
        if args.port == 8080:
            import urllib.request
            with open(args.rom, "rb") as fh:
                req = urllib.request.Request(
                    "http://127.0.0.1:8080/load_rom", data=fh.read(), method="POST")
            urllib.request.urlopen(req, timeout=30)
            print("ROM POSTed to web server")
            time.sleep(6.0)
        else:
            c0 = Client(port=args.port, path=args.path, players=args.players)
            c0.load_rom(args.rom)
            c0.close()
            print("ROM loaded via desktop WS")
            time.sleep(6.0)

    c = Client(port=args.port, path=args.path, players=args.players)
    f = c.frame(15.0)
    if f is None:
        print("FAIL: no frame after load")
        return 1
    print(f"boot frame: {len(f)} bytes")
    rate, n = c.fps(4.0)
    print(f"idle FPS: {rate:.1f} ({n} frames/4s)")

    for p in range(1, args.players + 1):
        print(f"--- navigating P{p} ---")
        ok = navigate(c, p, args.dump, args.tag)
        if not ok:
            print(f"FAIL: navigation of P{p} stalled")
        time.sleep(2.0)

    # Watch both players classify every second.
    state = {1: None, 2: None}
    deadline = time.time() + args.watch
    while time.time() < deadline:
        for p in range(1, args.players + 1):
            f = c.player_frame(p, 1.0)
            if f is not None:
                label, _ = classify(f)
                if label != state[p]:
                    state[p] = label
                    print(f"  [{int(time.time())}] P{p} screen: {label}")
                    save(f, f"{args.dump}/{args.tag}_p{p}_watch_{label}.png")
        time.sleep(1.0)

    print("state at end:", state)
    rate, n = c.fps(4.0)
    print(f"final FPS: {rate:.1f} ({n} frames/4s)")
    for p in range(1, args.players + 1):
        f = c.player_frame(p, 2.0)
        if f is not None:
            save(f, f"{args.dump}/{args.tag}_p{p}_final.png")

    bad = [p for p, s in state.items() if s == "multipak"]
    if bad:
        print(f"RESULT: FAIL - multi-pak warning on player(s) {bad}")
    elif all(s not in ("multipak",) and s is not None for s in state.values()):
        print("RESULT: both players past navigation (see final screenshots)")
    else:
        print("RESULT: unknown - inspect dumps")
    c.close()
    return 0 if not bad else 2


if __name__ == "__main__":
    sys.exit(main())