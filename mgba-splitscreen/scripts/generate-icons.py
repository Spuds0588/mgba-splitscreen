#!/usr/bin/env python3
"""Generate the PWA icon set from the source artwork (src/icon.svg).

GitHub Pages needs real PNGs: Chrome's installability check rejects SVG-only
manifest icons, and Android maskable launchers + iOS homescreens want raster
variants anyway. The artwork is simple geometry (rounded panel, cyan cross,
four player-color dots), so we redraw it with PIL instead of rasterizing the
SVG — same look, no extra tooling dependency.

Outputs (into mgba-splitscreen/src/):
  icon-192.png, icon-512.png        purpose=any
  icon-maskable-192.png, -512.png   purpose=maskable (artwork in the 80% safe zone)
  apple-touch-icon.png              180x180, iOS homescreen (full-bleed, no alpha)
  icon-32.png                       favicon fallback for browsers without SVG favicons
"""

from pathlib import Path

from PIL import Image, ImageDraw

SRC = Path(__file__).resolve().parent.parent / "src"

BG = (13, 13, 13, 255)        # #0d0d0d
PANEL = (28, 28, 31, 255)     # #1c1c1f
CYAN = (36, 200, 219, 255)    # #24c8db
DOTS = [  # (cx, cy, color) in the 512-space of icon.svg
    (138, 168, (240, 91, 104, 255)),   # P1 #f05b68
    (374, 168, (109, 169, 255, 255)),  # P2 #6da9ff
    (138, 316, (101, 209, 139, 255)),  # P3 #65d18b
    (374, 316, (240, 173, 85, 255)),   # P4 #f0ad55
]


def draw_art(scale: float) -> Image.Image:
    """Render the 512-space artwork scaled about the canvas center.

    scale < 1 shrinks the artwork for maskable variants; the caller paints a
    full-bleed background underneath either way.
    """
    size = 512
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    def pt(x: float, y: float) -> tuple[float, float]:
        return (size / 2 + (x - size / 2) * scale, size / 2 + (y - size / 2) * scale)

    def scaled(v: float) -> float:
        return v * scale

    # Panel with cyan border (rounded).
    x0, y0 = pt(54, 92)
    x1, y1 = pt(458, 392)
    d.rounded_rectangle([x0, y0, x1, y1], radius=scaled(30), fill=PANEL,
                        outline=CYAN, width=max(2, round(scaled(18))))

    # Cross lines at 80% opacity, clipped to the panel interior.
    overlay = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    od = ImageDraw.Draw(overlay)
    cx, cy = size / 2, size / 2
    top, bottom = pt(256, 94)[1], pt(256, 390)[1]
    left, right = pt(55, 242)[0], pt(457, 242)[0]
    w = max(2, round(scaled(12)))
    od.line([(cx, top), (cx, bottom)], fill=CYAN, width=w)
    od.line([(left, cy), (right, cy)], fill=CYAN, width=w)
    img = Image.alpha_composite(img, Image.eval(overlay, lambda v: int(v * 0.8)))
    d = ImageDraw.Draw(img)

    # Player dots.
    r = max(2, scaled(22))
    for cx_, cy_, color in DOTS:
        x, y = pt(cx_, cy_)
        d.ellipse([x - r, y - r, x + r, y + r], fill=color)
    return img


def any_icon(px: int) -> Image.Image:
    """purpose=any: rounded-corner app tile, transparent outside the corners."""
    img = Image.new("RGBA", (512, 512), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([0, 0, 511, 511], radius=96, fill=BG)
    out = Image.alpha_composite(img, draw_art(1.0))
    return out.resize((px, px), Image.LANCZOS)


def maskable_icon(px: int) -> Image.Image:
    """purpose=maskable: full-bleed background, artwork inside the 80% safe zone."""
    img = Image.new("RGBA", (512, 512), BG)
    out = Image.alpha_composite(img, draw_art(0.72))
    return out.resize((px, px), Image.LANCZOS)


def apple_touch_icon() -> Image.Image:
    """iOS: full-bleed square (iOS masks corners itself; alpha is stripped)."""
    img = Image.new("RGBA", (512, 512), BG)
    out = Image.alpha_composite(img, draw_art(0.9))
    return out.convert("RGB").resize((180, 180), Image.LANCZOS)


def main() -> None:
    any_icon(192).save(SRC / "icon-192.png", optimize=True)
    any_icon(512).save(SRC / "icon-512.png", optimize=True)
    maskable_icon(192).save(SRC / "icon-maskable-192.png", optimize=True)
    maskable_icon(512).save(SRC / "icon-maskable-512.png", optimize=True)
    apple_touch_icon().save(SRC / "apple-touch-icon.png", optimize=True)
    any_icon(32).save(SRC / "icon-32.png", optimize=True)
    for name in ("icon-192.png", "icon-512.png", "icon-maskable-192.png",
                 "icon-maskable-512.png", "apple-touch-icon.png", "icon-32.png"):
        p = SRC / name
        print(f"{name}: {p.stat().st_size} bytes")


if __name__ == "__main__":
    main()
