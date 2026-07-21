#!/usr/bin/env python3
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
"""Assert a viewer-dumped PPM actually rendered content.

This exists because "the viewer exited 0 and wrote a file" is not evidence that
anything was drawn. The viewer's failure modes both produce a perfectly valid
PPM: an all-one-color frame (the blank or error fill) and a zero-size image. A
real render must therefore have positive dimensions AND more than one distinct
color, and this checks both.

Distinct exit statuses let integration.sh tell a blank render (a real product
failure that must fail the run) apart from a bad invocation or a missing
Pillow, which must not be reported as a rendering bug:

    0  frame has content
    1  blank, degenerate, or unopenable frame
    2  usage error
    3  Pillow not installed

The color test is a floor, not a fingerprint: it catches blank output, not
wrong-but-colorful output.
"""

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
    """Check the PPM named in argv and map the verdict onto an exit status.

    Reads `FILE.ppm [MIN_COLORS]` from `sys.argv`. MIN_COLORS defaults to 2 --
    "not a single flat color" -- and raising it is how a caller demands a
    richer frame than merely non-blank.

    An unopenable file returns EXIT_BLANK rather than a usage error, on purpose:
    from integration.sh's point of view a frame the viewer never produced and a
    frame it produced blank are the same product failure.

    Color counting is capped at COLOR_CAP. Past that `getcolors()` returns None,
    which is itself proof of a rich frame, so the count is reported as the cap.

    Returns:
        EXIT_OK, EXIT_BLANK, or EXIT_USAGE, for `sys.exit`.

    Raises:
        ValueError: MIN_COLORS was given but is not an integer.
    """
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
