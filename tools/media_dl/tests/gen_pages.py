#!/usr/bin/env python3
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
"""Generate synthetic, non-copyright page images for the media_dl harness.

Feeds integration.sh. Every page is a distinct, high-contrast pattern, and both
properties are what make the downstream checks meaningful:

* Multiple colors per page, so `check_ppm.py`'s "did the viewer render
  anything" test can distinguish a real frame from a blank fill. A solid page
  would decode fine and prove nothing.
* Every page different from its neighbours, so an export that silently drops or
  duplicates a page cannot pass -- identical pages would compare equal.

No fonts, no network, no committed fixtures: the whole set is reproducible from
the command-line parameters alone, which is why this can run on any machine
without checking binaries into the tree.
"""

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
    """Render one synthetic page whose pixels encode its own index.

    Three overlaid features, each with a job:

    * A two-tone diagonal split, whose color is a function of `idx`. The large
      flat regions survive JPEG quantization, so the distinct-color count holds
      up after a lossy round trip.
    * A horizontal band whose vertical position interpolates `idx` across the
      page. This is what makes any two pages differ visibly.
    * A block "bar code" of the 1-based page number along the top edge, so a
      human looking at a dumped frame can read which page it is.

    Deterministic: the same arguments always produce identical pixels.

    Args:
        idx: 0-based page index; must be < `count` for the band to stay on the
            page.
        count: Total pages, used only to scale the band. A `count` of 1 pins
            the band to the top rather than dividing by zero.
        w: Page width in pixels.
        h: Page height in pixels. Both must be large enough for the derived
            feature sizes (`h // 20`, `w // 14`) to be non-zero.

    Returns:
        An RGB Pillow Image.
    """
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
    """Write COUNT synthetic pages into OUTDIR, from `OUTDIR COUNT W H` in argv.

    Pages are saved as baseline JPEG named `page_0001.jpg` upward, 1-based and
    zero-padded so a plain lexical sort matches page order. JPEG rather than PNG
    is deliberate: real scraped pages are JPEG, so this exercises the actual
    decode paths (the viewer probe and the jof producer) instead of a format
    they never see in production.

    OUTDIR must already exist; it is not created. Existing pages are
    overwritten, and pages from a previous larger run are NOT removed, so a
    reused directory can end up with stale extra pages.

    Returns:
        EXIT_OK, or EXIT_USAGE when too few arguments were given.

    Raises:
        ValueError: COUNT, W or H is not an integer.
        OSError: OUTDIR does not exist or is not writable.
    """
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
