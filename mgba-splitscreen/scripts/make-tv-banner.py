#!/usr/bin/env python3
"""Generate the Android TV launcher banner for the Android bundle.

Android TV launchers — and the Play Store's TV listing — want an
`android:banner` drawable that is exactly 320x180 with no transparency. This
draws one from the same palette the splitscreen UI uses for its four player
panels, so the TV tile reads as this app rather than as a scaled-up launcher
icon.

Usage (from anywhere):
    python3 mgba-splitscreen/scripts/make-tv-banner.py

Requires Pillow. Overwrites the drawable inside the generated Android project,
so re-run it after `tauri android init` regenerates that tree.
"""

from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

WIDTH, HEIGHT = 320, 180

# Matches the four player-panel accents in src/main.js / styles.css.
BACKGROUND = (16, 22, 29)
PLAYER_COLORS = [
    (224, 90, 90),    # P1 red
    (74, 144, 217),   # P2 blue
    (76, 175, 80),    # P3 green
    (224, 161, 58),   # P4 orange
]
TITLE_COLOR = (240, 244, 248)
SUBTITLE_COLOR = (140, 154, 168)

FONT_CANDIDATES = [
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
    "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
    "C:/Windows/Fonts/arialbd.ttf",
]
FONT_CANDIDATES_REGULAR = [p.replace("Bold", "") for p in FONT_CANDIDATES]


def load_font(candidates, size):
    for path in candidates:
        if Path(path).exists():
            try:
                return ImageFont.truetype(path, size)
            except OSError:
                continue
    return ImageFont.load_default()


def centered(draw, text, font, y, fill, width=WIDTH):
    left, top, right, bottom = draw.textbbox((0, 0), text, font=font)
    draw.text(((width - (right - left)) / 2 - left, y - top), text, font=font, fill=fill)


def main():
    out = (
        Path(__file__).resolve().parents[1]
        / "src-tauri/gen/android/app/src/main/res/drawable/tv_banner.png"
    )
    out.parent.mkdir(parents=True, exist_ok=True)

    img = Image.new("RGB", (WIDTH, HEIGHT), BACKGROUND)  # RGB: banners must be opaque
    draw = ImageDraw.Draw(img)

    # Four player strips along the bottom, the visual shorthand for the app.
    strip_h = 6
    segment = WIDTH / len(PLAYER_COLORS)
    for i, color in enumerate(PLAYER_COLORS):
        draw.rectangle(
            [round(i * segment), HEIGHT - strip_h, round((i + 1) * segment), HEIGHT],
            fill=color,
        )

    centered(draw, "mgba-splitscreen", load_font(FONT_CANDIDATES, 30), 62, TITLE_COLOR)
    centered(
        draw,
        "split-screen GBA link cable",
        load_font(FONT_CANDIDATES_REGULAR, 15),
        100,
        SUBTITLE_COLOR,
    )

    img.save(out, "PNG")
    print(f"wrote {out} ({out.stat().st_size} bytes, {WIDTH}x{HEIGHT})")


if __name__ == "__main__":
    main()
