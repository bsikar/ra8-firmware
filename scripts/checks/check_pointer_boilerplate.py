#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Reject generated pointer-only definition comments in apps and examples.

Application and example definitions inherit their contracts from declarations.
The exact ``see header for the documented contract`` sentence adds no useful
information and was emitted repeatedly during the source-layout migration.
Legacy library wording is outside this narrow regression guard.
"""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCOPED_PREFIXES = ("apps/", "examples/")
SOURCE_SUFFIXES = frozenset({".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".m", ".mm"})
MIN_SCOPED_FILES = 850
BANNED_RE = re.compile(
    r"^\s*/\*\s*see (?:the )?(?:internal )?header for the documented contract\.\s*\*/\s*$",
    re.IGNORECASE,
)


def is_banned(line: str) -> bool:
    """Return whether one source line is the generated pointer-only comment."""
    return BANNED_RE.fullmatch(line) is not None


def scoped_files() -> list[str]:
    """Return present tracked and untracked application/example source files."""
    git = shutil.which("git") or "git"
    result = subprocess.run(  # noqa: S603 -- resolved Git executable, fixed arguments
        [git, "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
        cwd=REPO_ROOT,
        check=True,
        capture_output=True,
    )
    selected = set()
    for rel in result.stdout.decode("utf-8", errors="strict").split("\0"):
        path = REPO_ROOT / rel
        if (
            rel.startswith(SCOPED_PREFIXES)
            and path.suffix.lower() in SOURCE_SUFFIXES
            and path.is_file()
        ):
            selected.add(rel)
    return sorted(selected)


def scan(rels: list[str]) -> list[str]:
    """Return every path and line carrying the banned generated sentence."""
    findings: list[str] = []
    for rel in rels:
        text = (REPO_ROOT / rel).read_text(encoding="utf-8")
        findings.extend(scan_text(rel, text))
    return findings


def scan_text(rel: str, text: str) -> list[str]:
    """Run the CI scanner over one named source text."""
    return [
        f"{rel}:{number}"
        for number, line in enumerate(text.splitlines(), start=1)
        if is_banned(line)
    ]


def selftest() -> int:
    """Drive the CI scanner through must-fire and must-stay-quiet source texts."""
    cases = (
        ("/* see header for the documented contract. */", True, "plain generated form fires"),
        (
            "/* See the internal header for the documented contract. */",
            True,
            "internal-header generated form fires",
        ),
        ("/* see header for full description */", False, "legacy wording stays quiet"),
        (
            "/* See header for the documented contract -- bounded scan. */",
            False,
            "an implementation-specific note stays quiet",
        ),
        (
            'const char* text = "see header for the documented contract.";',
            False,
            "a string literal stays quiet",
        ),
    )
    failures = [
        label
        for line, expected, label in cases
        if bool(scan_text("apps/example/src/main.c", line)) != expected
    ]
    if failures:
        for failure in failures:
            print(f"check_pointer_boilerplate.py --selftest: FAIL: {failure}", file=sys.stderr)
        return 1
    print(f"check_pointer_boilerplate.py --selftest: PASS ({len(cases)} both-direction cases)")
    return 0


def main() -> int:
    """Run the detector self-test or scan the live source tree."""
    if sys.argv[1:] == ["--selftest"]:
        return selftest()
    if sys.argv[1:]:
        print("usage: check_pointer_boilerplate.py [--selftest]", file=sys.stderr)
        return 2
    try:
        rels = scoped_files()
        findings = scan(rels)
    except (OSError, subprocess.CalledProcessError, UnicodeError) as exc:
        print(f"check_pointer_boilerplate.py: cannot scan source tree: {exc}", file=sys.stderr)
        return 2
    if len(rels) < MIN_SCOPED_FILES:
        print(
            f"check_pointer_boilerplate.py: scope collapsed to {len(rels)} file(s); "
            f"expected at least {MIN_SCOPED_FILES}",
            file=sys.stderr,
        )
        return 2
    if findings:
        print("Generated pointer-only definition comment(s):", file=sys.stderr)
        for finding in findings:
            print(f"  {finding}", file=sys.stderr)
        print("Delete the comment; the declaration owns the contract.", file=sys.stderr)
        return 1
    print(f"check_pointer_boilerplate.py: clean ({len(rels)} app/example source files)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
