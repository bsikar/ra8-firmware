#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: every first-party source file shall end in a trailing newline.

POSIX defines a text line as ending in a newline; tools that read the last
line (diff, cat, shell ``read``, parsers) behave better when the rule holds.
``.editorconfig`` declares ``insert_final_newline`` but only editors honour
it, and ``.clang-format``'s ``InsertNewlineAtEOF`` only covers C/C++ that
reaches the formatter.  Neither is a gate, and neither touches Python, shell,
CMake, or YAML.

This repo-wide backstop walks every first-party source file (C/C++, Python,
shell, CMake, YAML, linker scripts) under the scanned roots and fails if any
is non-empty and does not end in a newline byte.  Vendor trees and generated
font tables are excluded.  There is no grandfathering.

Run::

    check_final_newline.py                      # scan the whole tree
    check_final_newline.py path/to/file ...     # scan listed files

Exit 0 if every file ends in a newline, exit 1 (with a list) otherwise.
"""

from __future__ import annotations

import sys
from collections.abc import Iterable
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

SOURCE_SUFFIXES = (
    ".c",
    ".h",
    ".cpp",
    ".hpp",
    ".cc",
    ".cxx",
    ".hh",
    ".hxx",
    ".m",
    ".mm",
    ".inl",
    ".py",
    ".sh",
    ".cmake",
    ".yml",
    ".yaml",
    ".ld",
)
SOURCE_NAMES = ("CMakeLists.txt",)
SCAN_ROOTS = ("libs", "src", "port", "examples", "tools", "tests", "scripts", "cmake", ".github")
EXCLUDE_FRAGMENTS = (
    "libs/third_party/",
    "libs/fonts/",
    "port/threadx/",
    "/build/",
    "/build-cov/",
    "/_deps/",
    "_unsupported/",
    "__pycache__/",
    "node_modules/",
)


def _ends_in_newline(path: Path) -> bool:
    """Return True if `path` is empty or ends in a newline byte."""
    try:
        data = path.read_bytes()
    except OSError:
        return True  # unreadable: not this gate's problem
    return (not data) or data.endswith(b"\n")


def _is_excluded(path: Path) -> bool:
    return any(frag in str(path) for frag in EXCLUDE_FRAGMENTS)


def _is_source(path: Path) -> bool:
    return path.suffix in SOURCE_SUFFIXES or path.name in SOURCE_NAMES


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
                out.extend(c for c in path.rglob("*") if c.is_file() and _is_source(c))
            elif _is_source(path):
                out.append(path)
        return [p for p in out if not _is_excluded(p)]

    out = []
    for root in SCAN_ROOTS:
        base = REPO_ROOT / root
        if base.is_dir():
            out.extend(c for c in base.rglob("*") if c.is_file() and _is_source(c))
    return [p for p in out if not _is_excluded(p)]


def main(argv: list[str]) -> int:
    targets = _enumerate_targets(argv[1:])
    if not targets:
        print("check_final_newline.py: no files to scan", file=sys.stderr)
        return 0

    missing = sorted(_rel(p) for p in targets if not _ends_in_newline(p))
    if not missing:
        print(f"check_final_newline.py: {len(targets)} file(s) scanned, all end in a newline.")
        return 0

    print(
        f"check_final_newline.py: {len(missing)} file(s) missing a trailing newline:\n",
        file=sys.stderr,
    )
    for path in missing:
        print(f"  {path}", file=sys.stderr)
    print("\nAdd a single newline at end of file.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
