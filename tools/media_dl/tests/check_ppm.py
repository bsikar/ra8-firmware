#!/usr/bin/env python3
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
# Assert a viewer-dumped PPM actually rendered content. The viewer's failure
# modes are an all-one-color frame (blank/error fill) or a zero-size image, so a
# real render must have positive dimensions AND more than one distinct color.
# Exits 0 on a good frame, 1 on a blank/degenerate one, 2/3 on usage/dependency
# errors -- so integration.sh can treat a blank render as a hard failure.
import sys

try:
    from PIL import Image
except ImportError:
    sys.stderr.write("check_ppm.py: needs Pillow\n")
    sys.exit(3)


def main():
    if len(sys.argv) < 2:
        sys.stderr.write("usage: check_ppm.py FILE.ppm [MIN_COLORS]\n")
        return 2
    path = sys.argv[1]
    min_colors = int(sys.argv[2]) if len(sys.argv) > 2 else 2
    try:
        img = Image.open(path).convert("RGB")
    except Exception as exc:  # noqa: BLE001 -- report any decode failure verbatim
        sys.stderr.write("check_ppm: cannot open %s: %s\n" % (path, exc))
        return 1
    w, h = img.size
    if w <= 0 or h <= 0:
        sys.stderr.write("check_ppm: degenerate size %dx%d\n" % (w, h))
        return 1
    # getcolors returns None once the image has more colors than the cap, which
    # is itself proof of a rich (non-blank) frame.
    colors = img.getcolors(maxcolors=1 << 20)
    n = (1 << 20) if colors is None else len(colors)
    if n < min_colors:
        sys.stderr.write("check_ppm: BLANK %s (%dx%d, %d color(s))\n" % (path, w, h, n))
        return 1
    print("check_ppm: OK %s (%dx%d, %d+ colors)" % (path, w, h, n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
