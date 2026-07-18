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

# Exit statuses (integration.sh distinguishes a blank render from a bad call).
EXIT_OK = 0
EXIT_BLANK = 1
EXIT_USAGE = 2

# argv layout.
ARGV_MIN = 2
ARGV_WITH_MIN_COLORS = 3

# A frame must carry at least this many distinct colors to count as rendered.
DEFAULT_MIN_COLORS = 2

# getcolors() cap: past this it returns None, which is itself proof of a rich frame.
COLOR_CAP = 1 << 20


def main():
    if len(sys.argv) < ARGV_MIN:
        sys.stderr.write("usage: check_ppm.py FILE.ppm [MIN_COLORS]\n")
        return EXIT_USAGE
    path = sys.argv[1]
    min_colors = int(sys.argv[2]) if len(sys.argv) >= ARGV_WITH_MIN_COLORS else DEFAULT_MIN_COLORS
    try:
        img = Image.open(path).convert("RGB")
    except OSError as exc:
        sys.stderr.write(f"check_ppm: cannot open {path}: {exc}\n")
        return EXIT_BLANK
    w, h = img.size
    if w <= 0 or h <= 0:
        sys.stderr.write(f"check_ppm: degenerate size {w}x{h}\n")
        return EXIT_BLANK
    colors = img.getcolors(maxcolors=COLOR_CAP)
    n = COLOR_CAP if colors is None else len(colors)
    if n < min_colors:
        sys.stderr.write(f"check_ppm: BLANK {path} ({w}x{h}, {n} color(s))\n")
        return EXIT_BLANK
    print(f"check_ppm: OK {path} ({w}x{h}, {n}+ colors)")
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
