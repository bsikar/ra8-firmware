#!/usr/bin/env python3
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
# Generate synthetic, non-copyright page images for the media_dl integration
# harness (integration.sh). Every page is a distinct, high-contrast pattern so
# the downstream "did the viewer actually render something" check is meaningful:
# a solid page would decode fine yet tell us nothing, so each page here carries
# multiple colors AND differs from its neighbours (a per-page moving band + the
# page index rendered as a bar code of blocks). No fonts, no network, no repo
# fixtures -- fully reproducible from these parameters alone.
import sys

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.stderr.write("gen_pages.py: needs Pillow (pip install Pillow)\n")
    sys.exit(3)

# argv layout: OUTDIR COUNT W H (plus argv[0]).
ARGV_REQUIRED = 5

EXIT_OK = 0
EXIT_USAGE = 2

# Encoder quality for the synthetic baseline-JPEG pages.
JPEG_QUALITY = 90


def make_page(idx, count, w, h):
    # Two-tone diagonal split gives a large, JPEG-robust set of distinct colors.
    img = Image.new("RGB", (w, h), (250, 250, 250))
    d = ImageDraw.Draw(img)
    base = (40 + (idx * 37) % 160, 60 + (idx * 53) % 160, 90 + (idx * 71) % 160)
    d.polygon([(0, 0), (w, 0), (0, h)], fill=base)
    # A moving horizontal band whose position encodes the page index, so no two
    # pages decode to the same image (guards against an export that silently
    # duplicates or drops pages).
    band_y = int((h - h // 8) * (idx / max(1, count - 1))) if count > 1 else 0
    d.rectangle([0, band_y, w, band_y + h // 8], fill=(255, 200, 0))
    # A block "bar code" of the 1-based page number along the top edge.
    for bit in range(8):
        if (idx + 1) & (1 << bit):
            x = 10 + bit * (w // 10)
            d.rectangle([x, 10, x + w // 14, 10 + h // 20], fill=(0, 0, 0))
    return img


def main():
    if len(sys.argv) < ARGV_REQUIRED:
        sys.stderr.write("usage: gen_pages.py OUTDIR COUNT W H\n")
        return EXIT_USAGE
    outdir, count, w, h = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
    for i in range(count):
        page = make_page(i, count, w, h)
        # Baseline JPEG (what real scraped pages are) so the JPEG-decode paths
        # (viewer probe, jof producer) are exercised, not just PNG.
        page.save(f"{outdir}/page_{i + 1:04d}.jpg", "JPEG", quality=JPEG_QUALITY)
    print(f"gen_pages: wrote {count} page(s) ({w}x{h}) to {outdir}")
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
