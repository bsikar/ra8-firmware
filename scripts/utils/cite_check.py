#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""
cite_check.py -- validate Hardware User's Manual citations in source.

This script scans C / header files for in-line annotations of the form

    /* HUM Ch X.Y "section name", p NNNN */

and verifies that:

    1. The chapter number X exists in docs/reference/CHAPTER_MAP.md.
    2. The page number NNNN falls inside chapter X's page range.

Modes:

    --warn (default) -- exit 0, print findings to stderr.
    --strict (onward) -- exit 1 on any finding.

The script also accepts a list of explicit file arguments. With no
arguments, it scans the entire libs/, src/, and tests/ trees.

The chapter map is parsed from CHAPTER_MAP.md so the page-range
truth lives in exactly one place. The pre-commit hook invokes this
script after build_chapter_map.sh has been run; nothing here calls
pdftotext.

Citation format details:

    /* HUM Ch X.Y "..." p NNNN */ single-page form
    /* HUM Ch X.Y "..." p NNNN-MMMM */ page range form

The X chapter number is required; subsection Y is optional in the
parser (some module-stop-only writes only carry the chapter). The
section-name string is enforced as present so a future linter pass
can grep for human-readable context.

The script is intentionally conservative: an unparseable comment
that *looks* like a HUM cite (starts with `HUM Ch`) is reported as
malformed even if it would otherwise pass the page-in-range check.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
from typing import Iterable


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
CHAPTER_MAP_PATH = REPO_ROOT / "docs" / "reference" / "CHAPTER_MAP.md"

# `libs/`, `src/`, and `tests/` are always sources of truth. Per-app
# directories live under `examples/<name>/` and are discovered
# dynamically below.
ALWAYS_SCAN_DIRS = ("libs", "src", "tests")
SOURCE_SUFFIXES = {".c", ".h", ".cpp", ".hpp"}


def discover_scan_dirs() -> tuple[str, ...]:
    """Return (`libs`, `src`, `tests`) plus every example app dir.

    An "app dir" is any directory under `examples/` that contains
    both `main.c` and `CMakeLists.txt`.
    """
    out = list(ALWAYS_SCAN_DIRS)
    examples_root = REPO_ROOT / "examples"
    if examples_root.is_dir():
        for entry in sorted(examples_root.iterdir()):
            if not entry.is_dir():
                continue
            if (entry / "main.c").is_file() and (entry / "CMakeLists.txt").is_file():
                out.append(f"examples/{entry.name}")
    return tuple(out)


DEFAULT_SCAN_DIRS = discover_scan_dirs()

CITE_RE = re.compile(
    r"""
    /\*\s*HUM\s+Ch\s+
    (?P<chapter>\d{1,2}) # chapter number
    (?:\.(?P<sub>\d{1,3}(?:\.\d{1,3})*))? # optional subsection X.Y[.Z...]
    \s*
    "(?P<section>[^"]*)" # quoted section name
    \s*,?\s*
    p\s+
    (?P<start>\d{1,5}) # start page
    (?:\s*-\s*(?P<end>\d{1,5}))? # optional end page
    \s*\*/
    """,
    re.VERBOSE,
)

LOOSE_HUM_RE = re.compile(r"/\*\s*HUM\s+Ch\b[^*]*\*/")


def parse_chapter_map(path: pathlib.Path) -> dict[int, tuple[int, int, str]]:
    """Parse CHAPTER_MAP.md into {chapter: (start_page, end_page, title)}.

    The map's table rows look like:

        | 1 | Overview | 69 | 110 |

    Lines that don't match the row pattern are ignored.
    """
    if not path.exists():
        raise FileNotFoundError(f"chapter map missing: {path}")

    row_re = re.compile(
        r"^\|\s*(\d{1,2})\s*\|\s*([^|]+?)\s*\|\s*(\d{1,5})\s*\|\s*(\d{1,5})\s*\|\s*$"
    )

    chapters: dict[int, tuple[int, int, str]] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        m = row_re.match(line)
        if m is None:
            continue
        num = int(m.group(1))
        title = m.group(2).strip()
        start = int(m.group(3))
        end = int(m.group(4))
        if not (1 <= num <= 99):
            continue
        chapters[num] = (start, end, title)
    return chapters


def iter_source_files(targets: Iterable[pathlib.Path]) -> Iterable[pathlib.Path]:
    """Yield every C/H source file under any of the given targets."""
    for t in targets:
        if not t.exists():
            continue
        if t.is_file():
            if t.suffix.lower() in SOURCE_SUFFIXES:
                yield t
            continue
        for sub in t.rglob("*"):
            if not sub.is_file():
                continue
            if sub.suffix.lower() not in SOURCE_SUFFIXES:
                continue
            # Skip vendored / build trees: nothing under libs/third_party
            # or any build directory should be linted for HUM cites.
            parts = set(sub.parts)
            if "third_party" in parts or "build" in parts:
                continue
            if "_deps" in parts:
                continue
            yield sub


def check_file(
    path: pathlib.Path,
    chapters: dict[int, tuple[int, int, str]],
) -> list[str]:
    """Return a list of finding strings for one file."""
    findings: list[str] = []
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        return [f"{path}: read error: {exc}"]

    parsed_spans: list[tuple[int, int]] = []
    for m in CITE_RE.finditer(text):
        parsed_spans.append(m.span())
        chapter = int(m.group("chapter"))
        start = int(m.group("start"))
        end_raw = m.group("end")
        end = int(end_raw) if end_raw else start
        section = m.group("section")

        line_no = text.count("\n", 0, m.start()) + 1

        if chapter not in chapters:
            findings.append(
                f"{path}:{line_no}: HUM Ch {chapter} not in chapter map"
            )
            continue
        ch_start, ch_end, ch_title = chapters[chapter]

        if start < ch_start or end > ch_end:
            findings.append(
                f"{path}:{line_no}: HUM Ch {chapter} \"{section}\" "
                f"page range {start}-{end} outside chapter range "
                f"{ch_start}-{ch_end} ({ch_title})"
            )
            continue

        if end < start:
            findings.append(
                f"{path}:{line_no}: HUM Ch {chapter} reversed page range "
                f"{start}-{end}"
            )
            continue

    # Catch malformed HUM cites that did NOT match CITE_RE. Anything
    # under LOOSE_HUM_RE that wasn't already covered by a parsed span
    # is reported as malformed.
    for lm in LOOSE_HUM_RE.finditer(text):
        if any(s <= lm.start() < e for s, e in parsed_spans):
            continue
        line_no = text.count("\n", 0, lm.start()) + 1
        findings.append(
            f"{path}:{line_no}: malformed HUM cite "
            f"{lm.group(0)!r} -- expected /* HUM Ch X.Y \"...\" p NNNN */"
        )

    return findings


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "paths",
        nargs="*",
        help="files or directories to scan (default: libs/ src/ tests/)",
    )
    parser.add_argument(
        "--warn",
        action="store_true",
        help="warn-only mode: print findings, exit 0 (default)",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="strict mode: exit 1 on any finding (onward)",
    )
    parser.add_argument(
        "--chapter-map",
        default=str(CHAPTER_MAP_PATH),
        help=f"path to CHAPTER_MAP.md (default: {CHAPTER_MAP_PATH})",
    )
    args = parser.parse_args(argv)

    if args.warn and args.strict:
        print("cite_check.py: --warn and --strict are mutually exclusive",
              file=sys.stderr)
        return 2

    # Default to warn unless explicitly strict.
    strict = args.strict and not args.warn

    try:
        chapters = parse_chapter_map(pathlib.Path(args.chapter_map))
    except FileNotFoundError as exc:
        print(f"cite_check.py: {exc}", file=sys.stderr)
        return 2

    if args.paths:
        targets = [pathlib.Path(p) for p in args.paths]
    else:
        targets = [REPO_ROOT / d for d in DEFAULT_SCAN_DIRS]

    findings: list[str] = []
    file_count = 0
    for f in iter_source_files(targets):
        file_count += 1
        findings.extend(check_file(f, chapters))

    if findings:
        for line in findings:
            print(line, file=sys.stderr)
        verdict = "strict" if strict else "warn"
        print(
            f"cite_check.py: {len(findings)} finding(s) across {file_count} file(s) [{verdict}]",
            file=sys.stderr,
        )
        return 1 if strict else 0

    print(
        f"cite_check.py: 0 findings across {file_count} file(s)",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
