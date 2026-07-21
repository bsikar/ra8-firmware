#!/usr/bin/env python3
"""check-copyright.py -- enforce MIT SPDX + Brighton Sikarskie copyright
headers on every C / header / CMake / shell / python file.

Exits non-zero if any file is missing a header. Run from the pre-
commit hook against staged files:

    python3 scripts/checks/check-copyright.py path/to/file [more...]
"""

from __future__ import annotations

import pathlib
import sys

COPYRIGHT_SIGNATURES = (
    "Brighton Sikarskie",
    "SPDX-License-Identifier: MIT",
)

EXTENSIONS = {".c", ".h", ".cpp", ".hpp", ".cmake", ".sh", ".py"}

# Vendored / generated trees we don't author -- skip wholesale.
EXCLUDED_PARTS = {"third_party", "_deps", "build", "build-cov"}


def needs_header(path: pathlib.Path) -> bool:
    if path.suffix.lower() not in EXTENSIONS and path.name != "CMakeLists.txt":
        return False
    if path.is_dir():
        return False
    if not path.exists():
        return False
    return not any(part in EXCLUDED_PARTS for part in path.parts)


def check(path: pathlib.Path) -> bool:
    try:
        # Read the whole file: some file-level Doxygen blocks are long
        # enough that the copyright line sits past any fixed prefix.
        head = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False

    for needle in COPYRIGHT_SIGNATURES:
        if needle not in head:
            print(f"[MISSING] {path}: header does not contain '{needle}'", file=sys.stderr)
            return False
    return True


MIN_ARGS = 2  # script name + at least one file path


def main() -> int:
    if len(sys.argv) < MIN_ARGS:
        print("usage: check-copyright.py FILE [FILE ...]", file=sys.stderr)
        return 2

    failures = 0
    for raw in sys.argv[1:]:
        path = pathlib.Path(raw)
        if not needs_header(path):
            continue
        if not check(path):
            failures += 1

    return 0 if failures == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
