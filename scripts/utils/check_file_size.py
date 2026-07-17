#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: source files shall not exceed a maintainability line cap (1000).

The per-function NASA Power-of-10 Rule 4 cap (``check_function_size.py``,
60 lines/function) keeps individual routines small but says nothing about the
size of the *file* they live in: a translation unit can stay Rule-4-clean
while growing into a multi-thousand-line god-file, or the same body can be
copy-pasted across siblings until each is enormous.

This file-level backstop walks every ``.c``/``.h``/``.cpp``/``.hpp`` under
``libs/``, ``src/``, ``port/``, ``examples/``, ``tools/``, and ``tests/`` and
fails if any one file is over the cap.  Vendor trees (``libs/third_party/``,
``port/threadx/``), generated font tables (``libs/fonts/``), and generated
Vela model blobs (``tools/vela/generated/``) are excluded.

A file may waive the cap with an in-file marker in its head (mirrors the
repo's MAGIC-OK / CITES-OK family):
  @generated    -- machine-emitted; the generator owns its length, not us.
  FILE-SIZE-OK  -- explicit, justified waiver for a hand-authored file.

Run::

    check_file_size.py                      # scan the whole tree
    check_file_size.py path/to/file.c ...   # scan listed files

Exit 0 if every file is at or below the cap, exit 1 (with a table) otherwise.
"""

from __future__ import annotations

import sys
from collections.abc import Iterable
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# Maintainability cap: a single source file should stay reviewable.
THRESHOLD_LINES = 1000

SOURCE_SUFFIXES = (".c", ".h", ".cpp", ".hpp", ".cc", ".cxx", ".hh", ".hxx")
SCAN_ROOTS = ("libs", "src", "port", "examples", "tools", "tests")
EXCLUDE_FRAGMENTS = (
    "libs/third_party/",
    "libs/fonts/",
    "port/threadx/",
    "tools/vela/generated/",
    "/build/",
    "_unsupported/",
)

# In-file waiver markers; only the head is scanned so a stray occurrence deep
# in data cannot silently waive a real offender.
GENERATED_MARKERS = ("@generated", "FILE-SIZE-OK")
HEAD_SCAN_LINES = 40


def _is_waived(path: Path) -> bool:
    """Return True if `path`'s head carries a generated / size-OK marker."""
    try:
        with path.open("r", encoding="utf-8", errors="replace") as handle:
            for _ in range(HEAD_SCAN_LINES):
                line = handle.readline()
                if not line:
                    break
                if any(marker in line for marker in GENERATED_MARKERS):
                    return True
    except OSError:
        return False
    return False


def _count_lines(path: Path) -> int:
    """Return the physical line count of `path`, or -1 if unreadable."""
    try:
        text = path.read_text()
    except (OSError, UnicodeDecodeError):
        return -1
    return len(text.splitlines())


def _is_excluded(path: Path) -> bool:
    return any(frag in str(path) for frag in EXCLUDE_FRAGMENTS)


def _is_source(path: Path) -> bool:
    return path.suffix in SOURCE_SUFFIXES


def _rel(path: Path) -> str:
    if path.is_relative_to(REPO_ROOT):
        return str(path.relative_to(REPO_ROOT))
    return str(path)


def _enumerate_targets(arg_paths: Iterable[str]) -> list[Path]:
    """Resolve the list of files to scan from CLI arguments."""
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
        return [p for p in out if not _is_excluded(p)]

    out = []
    for root in SCAN_ROOTS:
        for suffix in SOURCE_SUFFIXES:
            out.extend((REPO_ROOT / root).rglob("*" + suffix))
    return [p for p in out if not _is_excluded(p)]


def main(argv: list[str]) -> int:
    targets = _enumerate_targets(argv[1:])
    if not targets:
        print("check_file_size.py: no files to scan", file=sys.stderr)
        return 0

    over = []
    for path in targets:
        if _is_waived(path):
            continue
        count = _count_lines(path)
        if count > THRESHOLD_LINES:
            over.append((count, _rel(path)))

    if not over:
        print(
            f"check_file_size.py: {len(targets)} file(s) scanned, "
            f"none over {THRESHOLD_LINES} lines."
        )
        return 0

    over.sort(reverse=True)
    print(
        f"check_file_size.py: {len(over)} file(s) exceed the {THRESHOLD_LINES}-line cap:\n",
        file=sys.stderr,
    )
    print("  lines  file", file=sys.stderr)
    for count, path in over:
        print(f"  {count:5d}  {path}", file=sys.stderr)
    print(
        "\nSplit the file along its responsibilities, or extract a shared "
        "helper if the bulk is duplicated.  Generated files may carry a "
        "@generated marker; a justified hand-authored file may use FILE-SIZE-OK.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
