#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Build a tiny demo .cbz from a few generated page images.

Draws three visually distinct portrait "comic" pages (a coloured gradient with a
large page number and framing) and zips them into sample.cbz, so the viewer has a
real archive to render for the rendering proof. Requires only Pillow.
"""

import zipfile
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).resolve().parent
PAGE_W, PAGE_H = 600, 900
PAGES = [
    ("page01.png", (240, 224, 200), (120, 60, 40), "1"),
    ("page02.png", (200, 224, 240), (30, 60, 120), "2"),
    ("page03.png", (210, 235, 210), (30, 110, 40), "3"),
]

# Candidate system faces, in preference order; falls back to Pillow's builtin.
FONT_CANDIDATES = (
    Path("/System/Library/Fonts/Supplemental/Arial Bold.ttf"),
    Path("/System/Library/Fonts/Helvetica.ttc"),
)

# Vertical shading range applied down each page, in 8-bit levels.
GRADIENT_DEPTH = 20


def _font(size):
    for path in FONT_CANDIDATES:
        if path.exists():
            return ImageFont.truetype(str(path), size)
    return ImageFont.load_default()


def make_page(bg, fg, label):
    """Render one portrait demo page: gradient, border, banner and page number.

    The output is intentionally busy. A flat page would render identically
    whether the viewer decoded it correctly, scaled it wrongly, or filled the
    frame with a solid error color; the gradient, the inset border and the
    360pt numeral each make a different class of rendering bug visible by eye.

    Not pixel-reproducible across machines: `_font` picks the first available
    system face and falls back to Pillow's builtin, so glyph shapes differ by
    host. Do not hash these pages -- they are a visual proof, not a golden.

    Args:
        bg: Background RGB triple. Each channel must be at least GRADIENT_DEPTH,
            or the shading underflows past 0 and Pillow rejects the color.
        fg: Foreground RGB triple for the border and all text.
        label: Page-number text, drawn centred at 360pt. Practically one or two
            characters; longer strings overflow the page.

    Returns:
        An RGB Pillow Image, PAGE_W by PAGE_H.
    """
    img = Image.new("RGB", (PAGE_W, PAGE_H), bg)
    draw = ImageDraw.Draw(img)
    for y in range(PAGE_H):
        shade = int(GRADIENT_DEPTH * (y / PAGE_H))
        draw.line(
            [(0, y), (PAGE_W, y)],
            fill=(bg[0] - shade, bg[1] - shade, bg[2] - shade),
        )
    draw.rectangle([20, 20, PAGE_W - 20, PAGE_H - 20], outline=fg, width=6)
    draw.text((PAGE_W // 2, 80), "RA8 VIEWER", font=_font(48), fill=fg, anchor="mm")
    draw.text((PAGE_W // 2, PAGE_H // 2), label, font=_font(360), fill=fg, anchor="mm")
    draw.text(
        (PAGE_W // 2, PAGE_H - 70),
        "sample comic page",
        font=_font(32),
        fill=fg,
        anchor="mm",
    )
    return img


def main():
    """Render every page in PAGES and zip them into sample.cbz beside this file.

    Output lands next to the script rather than in the working directory, so the
    fixture is regenerated in place wherever it is run from. An existing
    sample.cbz is overwritten.

    Each PNG is written to disk, added to the archive, then unlinked -- the
    intermediates are a zipfile.write() requirement, not artifacts, and only
    sample.cbz survives. A crash mid-loop leaves stray page*.png behind.

    Page order in the archive is PAGES order, which is what the viewer reads as
    reading order.
    """
    cbz = HERE / "sample.cbz"
    with zipfile.ZipFile(cbz, "w", zipfile.ZIP_DEFLATED) as zf:
        for name, bg, fg, label in PAGES:
            png = HERE / name
            make_page(bg, fg, label).save(png)
            zf.write(png, name)
            png.unlink()
    print("wrote", cbz)


if __name__ == "__main__":
    main()
