#!/usr/bin/env python3
"""Enforce the MIT SPDX and copyright header on every first-party source file.

Covers C, headers, CMake, shell and Python.

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
    """Whether this path is required to carry a licence header.

    Keyed on extension plus the exact basename ``CMakeLists.txt``, which has
    no distinguishing suffix and would otherwise escape the rule.
    """
    if path.suffix.lower() not in EXTENSIONS and path.name != "CMakeLists.txt":
        return False
    if path.is_dir():
        return False
    if not path.exists():
        return False
    return not any(part in EXCLUDED_PARTS for part in path.parts)


def check(path: pathlib.Path) -> bool:
    """Whether the file carries both the SPDX identifier and the copyright line.

    Reads the WHOLE file rather than a fixed-size head: some file-level
    Doxygen blocks in this tree are long enough that the copyright line sits
    past any prefix worth guessing, and a head-only read reported those as
    missing.

    Returns True when both are present.
    """
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
    """Check the files named on argv for their licence headers.

    Takes its targets from the caller (the pre-commit hook passes the staged
    list) and has no discovery mode, so an empty argv is a usage error exiting
    2 rather than a clean tree.

    Returns 0 when every named file is compliant, 1 on a missing header, 2 on
    the usage error.
    """
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
