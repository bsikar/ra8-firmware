#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""check-since-version.py -- enforce ``@since`` Doxygen tags AND
verify their values match the project ``VERSION`` file.

Two checks combined:

  1. **Presence**: every public declaration in a `.h` under
     ``libs/ra8_*/inc/`` (i.e. every ``ra8_*`` function or static
     inline accessor) must be preceded within the previous 30
     lines by a ``@since`` tag inside its Doxygen block.

  2. **Value**: every ``@since`` tag in any source / header /
     example / test file must use the exact version string in
     the project's top-level ``VERSION`` file. The tolerated
     variants are::

         @since 0.1.0
         @since Version 0.1.0   (legacy STAR-style; still accepted)

     Any other value is flagged.

Usage:

    # explicit file list (used by pre-commit hook):
    python3 scripts/checks/check-since-version.py path/to/file.h ...

    # full repo sweep (CI):
    python3 scripts/checks/check-since-version.py --all

The script always reads ``VERSION`` from the repo root, so a
single bump there propagates everywhere.

Exit code:
    0  no issues
    1  presence or value mismatch found
    2  CLI usage error
"""

from __future__ import annotations

import pathlib
import re
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
VERSION_FILE = REPO_ROOT / "VERSION"

ALWAYS_SCAN_DIRS = ("libs", "src", "tests")


def _discover_scan_dirs() -> tuple[str, ...]:
    """Return libs/src/tests + every examples/<tier>/<app>/ dir."""
    out = list(ALWAYS_SCAN_DIRS)
    examples_root = REPO_ROOT / "examples"
    if examples_root.is_dir():
        for tier in sorted(examples_root.iterdir()):
            if not tier.is_dir():
                continue
            for entry in sorted(tier.iterdir()):
                if not entry.is_dir():
                    continue
                if (entry / "main.c").is_file() and (entry / "CMakeLists.txt").is_file():
                    out.append(f"examples/{tier.name}/{entry.name}")
    return tuple(out)


SCAN_DIRS = _discover_scan_dirs()
SOURCE_SUFFIXES = {".c", ".h", ".cpp", ".hpp"}

PUBLIC_DECL = re.compile(
    r"""
    ^(?:\[\[nodiscard\]\]\s+)?
    (?:static\s+inline\s+)?
    \s*ra8_\w+(?:\s*\*)?\s+
    (ra8_\w+)\s*
    \(
""",
    re.VERBOSE,
)

SINCE_TAG_PRESENT = re.compile(r"@since")
# Match ``@since 1.2.3`` or ``@since Version 1.2.3``; capture the version.
SINCE_VALUE = re.compile(r"@since\s+(?:Version\s+)?([0-9]+(?:\.[0-9]+){1,2}[a-z]?)")


def read_project_version() -> str:
    if not VERSION_FILE.is_file():
        msg = f"error: {VERSION_FILE} missing -- create it with a single semver line"
        raise SystemExit(msg)
    text = VERSION_FILE.read_text(encoding="utf-8").strip()
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", text):
        msg = f"error: {VERSION_FILE} content '{text}' is not semver MAJOR.MINOR.PATCH"
        raise SystemExit(msg)
    return text


def check_presence(path: pathlib.Path) -> list[str]:
    """Header-only: every ra8_* declaration must have @since in lookback."""
    problems: list[str] = []
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeDecodeError):
        return problems

    for i, line in enumerate(lines):
        match = PUBLIC_DECL.match(line)
        if not match:
            continue
        lookback = "\n".join(lines[max(0, i - 30) : i])
        if not SINCE_TAG_PRESENT.search(lookback):
            problems.append(f"{path}:{i + 1}: {match.group(1)} missing @since")
    return problems


def check_values(path: pathlib.Path, project_version: str) -> list[str]:
    """All sources: every @since's value must match project_version."""
    problems: list[str] = []
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        return problems

    for line_no, line in enumerate(text.splitlines(), start=1):
        m = SINCE_VALUE.search(line)
        if not m:
            continue
        if m.group(1) != project_version:
            problems.append(f"{path}:{line_no}: @since {m.group(1)} != project {project_version}")
    return problems


def is_under_lib_inc(path: pathlib.Path) -> bool:
    return "libs/ra8_" in str(path) and path.suffix == ".h" and "/inc/" in str(path)


def is_scannable(path: pathlib.Path) -> bool:
    if not path.is_file():
        return False
    if path.suffix not in SOURCE_SUFFIXES:
        return False
    rel = path.relative_to(REPO_ROOT).as_posix()
    if any(rel.startswith(d + "/") or rel == d for d in ("build", "fsp", "STAR")):
        return False
    return any(rel.startswith(d + "/") for d in SCAN_DIRS)


def collect_repo_paths() -> list[pathlib.Path]:
    out: list[pathlib.Path] = []
    for d in SCAN_DIRS:
        root = REPO_ROOT / d
        if root.is_dir():
            out.extend(p for p in root.rglob("*") if is_scannable(p))
    return out


def main() -> int:
    project_version = read_project_version()

    if len(sys.argv) >= 2 and sys.argv[1] == "--all":  # noqa: PLR2004  # argv[1] presence check
        paths = collect_repo_paths()
    elif len(sys.argv) >= 2:  # noqa: PLR2004  # argv[1] presence check
        paths = [pathlib.Path(p).resolve() for p in sys.argv[1:]]
    else:
        print("usage: check-since-version.py FILE [FILE ...] | --all", file=sys.stderr)
        return 2

    failures: list[str] = []
    for path in paths:
        if not path.is_file():
            continue
        if is_under_lib_inc(path):
            failures.extend(check_presence(path))
        if path.suffix in SOURCE_SUFFIXES:
            failures.extend(check_values(path, project_version))

    if failures:
        print(f"check-since-version.py: project version is {project_version}", file=sys.stderr)
        for line in failures:
            print(line, file=sys.stderr)
        print(f"\n{len(failures)} issue(s) found.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
