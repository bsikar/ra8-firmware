#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""fix-encoding.py -- normalise source files to pure 7-bit ASCII.

Walks a file or directory, replacing common non-ASCII Unicode
characters with their ASCII equivalents (em-dash -> --, smart
quotes -> plain quotes, Greek mu -> u, etc.) so the pre-commit
hook stops rejecting the commit.

Usage:
    python3 scripts/fix/fix-encoding.py path/to/file
    python3 scripts/fix/fix-encoding.py path/to/dir
    python3 scripts/fix/fix-encoding.py --check path/to/dir  # report only
"""

from __future__ import annotations

import argparse
import pathlib
import sys

# U+NNNN -> ASCII replacement. Keep this list in sync with the
# rx72n project's equivalent script so both trees use the same
# canonical replacements.
REPLACEMENTS: dict[str, str] = {
    "\u2014": "--",  # em dash
    "\u2013": "-",  # en dash
    "\u2018": "'",  # left single quote
    "\u2019": "'",  # right single quote
    "\u201c": '"',  # left double quote
    "\u201d": '"',  # right double quote
    "\u2026": "...",  # ellipsis
    "\u00a0": " ",  # non-breaking space
    "\u00b0": " deg",  # degree sign
    "\u00b1": "+/-",  # plus-minus
    "\u00b5": "u",  # micro
    "\u03bc": "u",  # Greek mu
    "\u2264": "<=",  # less than or equal
    "\u2265": ">=",  # greater than or equal
    "\u2260": "!=",  # not equal
    "\u2192": "->",  # right arrow
    "\u2190": "<-",  # left arrow
    "\u00d7": "x",  # times
    "\u00f7": "/",  # divide
}


EXTENSIONS = {
    ".c",
    ".h",
    ".cpp",
    ".hpp",
    ".dox",
    ".md",
    ".yml",
    ".yaml",
    ".sh",
    ".py",
    ".cmake",
    ".json",
    ".toml",
    ".cfg",
    ".conf",
    ".tex",
    ".txt",
    ".ini",
    ".ld",
    ".s",
    ".m",
}


# Vendored / generated trees we don't author -- skip wholesale.
# Mirrors check_no_ai_attribution.py and check_line_citations.py.
EXCLUDED_PARTS = {"third_party", "_deps", "build", "build-cov", "doxygen_theme", "fixtures"}

# Largest code point in 7-bit ASCII (U+007F).
MAX_ASCII_CODEPOINT = 127


def rewrite(text: str) -> tuple[str, int]:
    """Return replaced text and the number of substitutions."""
    count = 0
    for bad, good in REPLACEMENTS.items():
        if bad in text:
            count += text.count(bad)
            text = text.replace(bad, good)
    # Any remaining non-ASCII character becomes '?'.
    cleaned = []
    for ch in text:
        if ord(ch) <= MAX_ASCII_CODEPOINT:
            cleaned.append(ch)
        else:
            cleaned.append("?")
            count += 1
    return "".join(cleaned), count


def process(path: pathlib.Path, *, check_only: bool) -> int:
    try:
        original = path.read_text(encoding="utf-8")
    except (UnicodeDecodeError, OSError) as exc:
        print(f"[SKIP] {path}: {exc}", file=sys.stderr)
        return 0

    replaced, changed = rewrite(original)
    if changed == 0:
        return 0

    if check_only:
        print(f"[NEEDS-FIX] {path}: {changed} non-ASCII characters")
    else:
        path.write_text(replaced, encoding="utf-8")
        print(f"[FIXED] {path}: {changed} replacements")
    return changed


def walk(target: pathlib.Path, *, check_only: bool) -> int:
    if target.is_file():
        return process(target, check_only=check_only)
    total = 0
    for path in target.rglob("*"):
        if not path.is_file():
            continue
        if path.suffix.lower() not in EXTENSIONS:
            continue
        if any(part in EXCLUDED_PARTS for part in path.parts):
            continue
        total += process(path, check_only=check_only)
    return total


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", type=pathlib.Path)
    parser.add_argument("--check", action="store_true", help="Only report, do not modify")
    args = parser.parse_args()

    changed = walk(args.target, check_only=args.check)
    if args.check and changed:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
