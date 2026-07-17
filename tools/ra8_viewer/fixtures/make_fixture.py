#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Build a tiny demo .cbz from a few generated page images.

Draws three visually distinct portrait "comic" pages (a coloured gradient with a
large page number and framing) and zips them into sample.cbz, so the viewer has a
real archive to render for the rendering proof. Requires only Pillow.
"""
import os
import zipfile

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
PAGE_W, PAGE_H = 600, 900
PAGES = [
    ("page01.png", (240, 224, 200), (120, 60, 40), "1"),
    ("page02.png", (200, 224, 240), (30, 60, 120), "2"),
    ("page03.png", (210, 235, 210), (30, 110, 40), "3"),
]


def _font(size):
    for path in (
        "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
    ):
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def make_page(bg, fg, label):
    img = Image.new("RGB", (PAGE_W, PAGE_H), bg)
    draw = ImageDraw.Draw(img)
    for y in range(PAGE_H):
        shade = int(20 * (y / PAGE_H))
        draw.line([(0, y), (PAGE_W, y)], fill=(bg[0] - shade, bg[1] - shade, bg[2] - shade))
    draw.rectangle([20, 20, PAGE_W - 20, PAGE_H - 20], outline=fg, width=6)
    draw.text((PAGE_W // 2, 80), "RA8 VIEWER", font=_font(48), fill=fg, anchor="mm")
    draw.text((PAGE_W // 2, PAGE_H // 2), label, font=_font(360), fill=fg, anchor="mm")
    draw.text((PAGE_W // 2, PAGE_H - 70), "sample comic page", font=_font(32), fill=fg, anchor="mm")
    return img


def main():
    cbz = os.path.join(HERE, "sample.cbz")
    with zipfile.ZipFile(cbz, "w", zipfile.ZIP_DEFLATED) as zf:
        for name, bg, fg, label in PAGES:
            png = os.path.join(HERE, name)
            make_page(bg, fg, label).save(png)
            zf.write(png, name)
            os.remove(png)
    print("wrote", cbz)


if __name__ == "__main__":
    main()
