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
    check_example_board_pins.py --selftest          # prove both directions

Exit 0 if no example hand-encodes a board pin, exit 1 (with the offenders)
otherwise, exit 2 when the whole-tree sweep collapses below FILE_FLOOR.
"""

from __future__ import annotations

import re
import sys
from collections.abc import Iterable
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from lint_targets import is_build_output_path  # needs the sys.path line above
from selftest_assert import expect, report  # needs the sys.path line above

REPO_ROOT = Path(__file__).resolve().parents[2]

SOURCE_SUFFIXES = (".c", ".h", ".cpp", ".hpp")
SCAN_ROOT = "examples"

# ((uint16_t)k_ra8_port_N << 8) | (uint16_t)k_ra8_pin_M, tolerant of spacing.
ENCODING_RE = re.compile(r"k_ra8_port_\d+\s*<<\s*8\s*\)\s*\|\s*\(\s*uint16_t\s*\)\s*k_ra8_pin_\d+")

# An examples/ tree this size cannot legitimately collapse to a handful of
# files. If the whole-tree sweep returns less than this, something broke (an
# unreachable repo root, a renamed SCAN_ROOT) and reporting "none hand-encode a
# board pin" would be a lie: the idiom cannot be found in a file nobody read.
# Measured 2026-07-28: 408 example sources. Same trip-wire as check_ruff.py.
#
# The count is not reproducible unless build output is excluded: a configured
# in-source build under examples/<app>/build/ inflated the sweep 408 -> 409
# (#549). `is_build_output_path` filters those, matching every peer checker, so
# what the gate scans is the committed source and nothing generated on top.
FILE_FLOOR = 320


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
        return [p for p in out if not is_build_output_path(p)]

    out = []
    for suffix in SOURCE_SUFFIXES:
        out.extend((REPO_ROOT / SCAN_ROOT).rglob("*" + suffix))
    return [p for p in out if not is_build_output_path(p)]


def selftest() -> int:
    """Prove the detector fires on the idiom and that build output is excluded.

    Three things have to hold at once for a clean run to mean anything: the
    encoding regex must FIRE on the ``(port << 8) | pin`` shape and stay QUIET
    on a board-symbol reference, the enumeration must DROP an in-source build
    file (the scope defect #549 fixed) while KEEPING a real example source, and
    the live sweep must clear ``FILE_FLOOR`` so a collapsed scope cannot pass as
    a clean tree.

    Returns:
        0 when every assertion held in both directions, 1 otherwise.
    """
    failures: list[str] = []

    idiom = "  cfg.pin = ((uint16_t)k_ra8_port_6 << 8) | (uint16_t)k_ra8_pin_11;"
    board_ref = "  cfg.pin = ra8_board_sw_pin(k_ra8_board_sw_user);"
    expect(bool(ENCODING_RE.search(idiom)), "MUST FIRE: hand-encoded (port << 8) | pin", failures)
    expect(
        not ENCODING_RE.search(board_ref),
        "MUST NOT FIRE: a board-symbol reference",
        failures,
    )

    enumerated = {
        str(p) for p in _enumerate_targets(["examples/x/build/gen.c", "examples/x/src/main.c"])
    }
    expect(
        any(p.endswith("examples/x/src/main.c") for p in enumerated),
        "MUST FIRE: a real example source is enumerated",
        failures,
    )
    expect(
        not any(p.endswith("examples/x/build/gen.c") for p in enumerated),
        "MUST NOT FIRE: an in-source build file is excluded from the scope",
        failures,
    )

    live = _enumerate_targets([])
    expect(
        len(live) >= FILE_FLOOR,
        f"live sweep sees {len(live)} example file(s) (floor {FILE_FLOOR})",
        failures,
    )
    expect(
        all(not is_build_output_path(p) for p in live),
        "no enumerated file lives in a build tree",
        failures,
    )
    return report(failures)


def main(argv: list[str]) -> int:
    """Fail any example that spells a board pin as an inline ``(port << 8) | pin``.

    The match is textual, on the encoding SHAPE rather than on known pin
    values, because the defect #251 found was the same four USB-FS pins
    copy-pasted across 29 apps under a dozen different local names -- a value
    allowlist would have had to know all twelve names, while the shift-or
    pattern catches the next one regardless of what it is called.

    Undecodable bytes are replaced rather than raising, so one bad file cannot
    abort the sweep; encoding is check-encoding's gate, not this one's.

    FILE_FLOOR applies to the whole-tree sweep ONLY, and exits 2 below it. An
    argv file list comes from the pre-commit hook and legitimately filters to
    nothing when a commit touches no example source, so an empty list there is
    a real answer; an empty SWEEP is a broken enumeration reporting a clean
    tree because it read nothing.

    Returns 1 listing each site, 0 when clean or when argv filtered to nothing,
    2 when the whole-tree sweep enumerated too few files to trust.
    """
    if "--selftest" in argv[1:]:
        return selftest()
    paths = argv[1:]
    targets = _enumerate_targets(paths)
    if not paths and len(targets) < FILE_FLOOR:
        print(
            f"check_example_board_pins.py: FATAL -- only {len(targets)} example file(s) "
            f"in scope, floor is {FILE_FLOOR}. A collapsed sweep reports a clean tree "
            "because it scanned nothing.",
            file=sys.stderr,
        )
        return 2
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
