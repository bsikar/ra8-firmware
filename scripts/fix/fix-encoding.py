#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""fix-encoding.py -- normalise source files to pure 7-bit ASCII.

Replaces common non-ASCII Unicode characters with their ASCII equivalents
(em-dash -> --, smart quotes -> plain quotes, Greek mu -> u, etc.), and is the
detector behind the ``ascii`` CI gate and ``make ascii``.

``--all`` is the gate mode and DERIVES its scope from ``git ls-files`` via
``lint_targets.first_party_paths()``.  It used to be driven by a hardcoded root
list in two places -- ``gate_ascii`` and ``mk/quality.mk`` both looped over
``src libs tests examples port scripts tools docs`` -- so the encoding policy
never saw the repo root, ``.github/``, ``cmake/``, ``coprocessor/``, ``infra/``
or ``mk/``.  106 files, including ``CLAUDE.md``, the file that *states* the
policy (#533).  A derived scope puts a new top-level directory in the gate the
day it lands, with no list to remember.

Aggravating the old form: a path that does not exist used to produce a silent
0, because ``rglob`` over a missing directory yields nothing -- so renaming a
root would have turned the gate green rather than red.  A missing target is now
a hard error, and ``--all`` carries a non-vacuity floor.

Usage:
    python3 scripts/fix/fix-encoding.py --all            # rewrite in place
    python3 scripts/fix/fix-encoding.py --check --all    # the gate mode
    python3 scripts/fix/fix-encoding.py --check path/to/file_or_dir
    python3 scripts/fix/fix-encoding.py --selftest       # both directions

Exit 0 if clean, 1 when a non-ASCII character was found, 2 when the scan could
not be trusted (a missing target, a collapsed enumeration, a failed selftest).
"""

from __future__ import annotations

import argparse
import pathlib
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "checks"))

from lint_targets import first_party_paths

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

# Non-vacuity floor for --all. The derived first-party set holds 3388 files
# with these suffixes today (2026-07-28); this is not a per-file target but a
# trip-wire for a scope that collapses wholesale, which would report the whole
# tree ASCII-clean having read almost none of it. Same shape as check_ruff.py
# and check_lint_coverage.py.
FILE_FLOOR = 2500

EXIT_OK = 0
EXIT_FINDINGS = 1
EXIT_VACUOUS = 2


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
    """Transliterate one file's non-ASCII characters, or just count them.

    A file that cannot be decoded as UTF-8 is SKIPPED with a message rather
    than failed: it is almost certainly binary, and rewriting it would corrupt
    it.

    Returns the number of offending characters found (0 when clean).
    """
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
    """Process one file, or recurse through a directory.

    Returns the total count of non-ASCII characters across everything visited.
    """
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


def derived_targets() -> list[pathlib.Path]:
    """Every first-party file the encoding policy covers, derived from git.

    Returns:
        Repo-relative paths with a text suffix this script understands.

    Raises:
        SystemExit: Via `first_party_paths` when ``git ls-files`` collapses.
    """
    return [pathlib.Path(rel) for rel in first_party_paths(tuple(sorted(EXTENSIONS)))]


def _run_all(*, check_only: bool) -> int:
    """Scan the derived first-party set, refusing a collapsed enumeration.

    Args:
        check_only: Report without writing.

    Returns:
        A process exit status.
    """
    targets = derived_targets()
    if len(targets) < FILE_FLOOR:
        print(
            f"fix-encoding.py: FATAL -- only {len(targets)} file(s) in scope, floor is "
            f"{FILE_FLOOR}. A collapsed scope reports a clean tree because it read nothing.",
            file=sys.stderr,
        )
        return EXIT_VACUOUS
    changed = sum(process(path, check_only=check_only) for path in targets)
    print(f"fix-encoding.py: {changed} non-ASCII character(s) across {len(targets)} file(s)")
    return EXIT_FINDINGS if (check_only and changed) else EXIT_OK


def selftest() -> int:
    """Assert the detector fires on a non-ASCII byte and stays quiet on ASCII.

    The quiet direction alone proves nothing -- a checker whose scope had
    collapsed to zero files is also perfectly quiet. Both directions plus the
    live-scope floor are what make a clean run mean something.

    Returns:
        ``EXIT_OK`` when every case holds, ``EXIT_VACUOUS`` otherwise.
    """
    cases: list[tuple[str, bool]] = []
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        clean = root / "clean.md"
        clean.write_text("plain ASCII -- nothing exotic\n", encoding="utf-8")
        dirty = root / "dirty.md"
        # Written as escapes on purpose: this file is itself inside the scope
        # of the policy it enforces, so a literal em-dash here would make the
        # detector fail on its own source.
        dirty.write_text("an em\u2014dash and a \u00b5\n", encoding="utf-8")

        cases.append(("MUST NOT FIRE: a pure-ASCII file", process(clean, check_only=True) == 0))
        cases.append(
            ("MUST FIRE: an em-dash and a micro sign", process(dirty, check_only=True) > 0)
        )
        cases.append(
            (
                "MUST FIRE: a directory walk reaches the offending file",
                walk(root, check_only=True) > 0,
            )
        )
        process(dirty, check_only=False)
        cases.append(
            ("MUST NOT FIRE: the rewritten file is clean", process(dirty, check_only=True) == 0)
        )
        cases.append(
            (
                "rewrite transliterated rather than deleted",
                dirty.read_text(encoding="utf-8") == "an em--dash and a u\n",
            )
        )

        missing = root / "no-such-dir"
        cases.append(
            (
                "MUST FIRE: a target that does not exist is an error, not a silent 0",
                not missing.exists() and walk_or_fail(missing, check_only=True) is None,
            )
        )

    cases.append(("the live derived scope clears the floor", len(derived_targets()) >= FILE_FLOOR))
    for label, ok in cases:
        print(f"  {'ok  ' if ok else 'FAIL'} {label}")
    if not all(ok for _, ok in cases):
        print("fix-encoding.py: selftest FAILED", file=sys.stderr)
        return EXIT_VACUOUS
    print(f"fix-encoding.py: selftest passed ({len(cases)} cases, both directions).")
    return EXIT_OK


def walk_or_fail(target: pathlib.Path, *, check_only: bool) -> int | None:
    """Walk `target`, or return None when it does not exist.

    A missing path used to walk to zero findings and exit 0, so renaming a
    scanned root would have turned the gate green rather than red (#533).

    Args:
        target: File or directory to scan.
        check_only: Report without writing.

    Returns:
        The non-ASCII character count, or None when `target` is absent.
    """
    if not target.exists():
        return None
    return walk(target, check_only=check_only)


def main() -> int:
    """Rewrite non-ASCII characters to ASCII equivalents, or check for them.

    ``--check`` reports without writing, which is the gate mode; bare, it
    rewrites in place.  Exactly one of ``--all``, ``--selftest`` or a positional
    target is required: a bare invocation used to be an argparse error only by
    accident of ``target`` being positional, and the gate mode has to be named
    explicitly so it cannot be entered by omission.

    Returns:
        0 when clean, 1 when a non-ASCII character was found in ``--check``
        mode, 2 when the scan itself could not be trusted.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", type=pathlib.Path, nargs="?")
    parser.add_argument("--check", action="store_true", help="Only report, do not modify")
    parser.add_argument(
        "--all",
        action="store_true",
        help="scan the derived first-party set (the gate mode)",
    )
    parser.add_argument("--selftest", action="store_true", help="prove both directions, then exit")
    args = parser.parse_args()

    if args.selftest:
        return selftest()
    if args.all == (args.target is not None):
        parser.error("pass exactly one of --all or a target path")
    if args.all:
        return _run_all(check_only=args.check)

    changed = walk_or_fail(args.target, check_only=args.check)
    if changed is None:
        print(f"fix-encoding.py: FATAL -- '{args.target}' does not exist.", file=sys.stderr)
        return EXIT_VACUOUS
    return EXIT_FINDINGS if (args.check and changed) else EXIT_OK


if __name__ == "__main__":
    raise SystemExit(main())
