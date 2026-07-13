#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: examples shall not hand-encode board connector pins.

The EK-RA8D2 pinout is a fixed board fact, and the board layer
(``libs/ra8_board_ek_ra8d2``) is its single source of truth: it exposes the
USB, I2C/I3C, SD, Ethernet, audio, console, LED and switch pins as
``k_ra8_board_*`` symbols (and accessors like ``ra8_board_sw_pin``). When an
application instead re-encodes a pin as ``(port << 8) | pin`` it duplicates
that fact -- and #251 showed the cost: the four USB-FS pins were copy-pasted,
byte-identical, across 29 apps under a dozen different local names, so a pin
correction in one silently skipped the rest.

This gate forbids the ``((uint16_t)k_ra8_port_N << 8) | (uint16_t)k_ra8_pin_M``
board-pin encoding idiom anywhere under ``examples/``. The fix is always to
reference the board symbol, or -- if the pin is a real board connector the
board layer does not expose yet -- to add it there first, then reference it.

Scope note: this targets the specific *encoding* idiom that was duplicated,
not every pin literal. A genuinely app-specific pin (e.g. where an external
motor driver is wired, which is not an EK-RA8D2 board fact and has no board
home) is out of scope by construction -- it does not use this idiom.

Run::

    check_example_board_pins.py                     # scan examples/
    check_example_board_pins.py path/to/main.c ...  # scan listed files

Exit 0 if no example hand-encodes a board pin, exit 1 (with the offenders)
otherwise.
"""

from __future__ import annotations

import re
import sys
from collections.abc import Iterable
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

SOURCE_SUFFIXES = (".c", ".h", ".cpp", ".hpp")
SCAN_ROOT = "examples"

# ((uint16_t)k_ra8_port_N << 8) | (uint16_t)k_ra8_pin_M, tolerant of spacing.
ENCODING_RE = re.compile(r"k_ra8_port_\d+\s*<<\s*8\s*\)\s*\|\s*\(\s*uint16_t\s*\)\s*k_ra8_pin_\d+")


def _is_source(path: Path) -> bool:
    return path.suffix in SOURCE_SUFFIXES


def _rel(path: Path) -> str:
    if path.is_relative_to(REPO_ROOT):
        return str(path.relative_to(REPO_ROOT))
    return str(path)


def _enumerate_targets(arg_paths: Iterable[str]) -> list[Path]:
    args = list(arg_paths)
    if args:
        out: list[Path] = []
        for raw in args:
            path = Path(raw)
            if not path.is_absolute():
                path = REPO_ROOT / path
            if path.is_dir():
                for suffix in SOURCE_SUFFIXES:
                    out.extend(path.rglob("*" + suffix))
            elif _is_source(path):
                out.append(path)
        return out

    out = []
    for suffix in SOURCE_SUFFIXES:
        out.extend((REPO_ROOT / SCAN_ROOT).rglob("*" + suffix))
    return out


def main(argv: list[str]) -> int:
    targets = _enumerate_targets(argv[1:])
    if not targets:
        print("check_example_board_pins.py: no files to scan", file=sys.stderr)
        return 0

    hits = []
    for path in targets:
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for lineno, line in enumerate(text.splitlines(), 1):
            if ENCODING_RE.search(line):
                hits.append((_rel(path), lineno, line.strip()))

    if not hits:
        print(
            f"check_example_board_pins.py: {len(targets)} example file(s) "
            "scanned, none hand-encode a board pin."
        )
        return 0

    print(
        f"check_example_board_pins.py: {len(hits)} hand-encoded board pin(s) in examples:\n",
        file=sys.stderr,
    )
    for path, lineno, snippet in hits:
        print(f"  {path}:{lineno}  {snippet}", file=sys.stderr)
    print(
        "\nThe EK-RA8D2 pinout belongs to the board layer, not to each app.\n"
        "Reference the board symbol (k_ra8_board_*_pin_*, or an accessor like\n"
        "ra8_board_sw_pin) instead of re-encoding (port << 8 | pin). If the pin\n"
        "is a real board connector the board layer does not expose yet, add it\n"
        "to libs/ra8_board_ek_ra8d2 first, then reference it here.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
