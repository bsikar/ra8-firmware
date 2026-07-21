#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""check_obsolete_standards.py -- Reject references to obsolete safety standards.

Per the project's IEC 61508 SIL 3 / DO-178C target (CLAUDE.md):

- DO-178B was superseded by DO-178C in December 2011 and is no longer
  recognized by FAA / EASA / Transport Canada. Any new doc, test
  comment, or commit message that says "DO-178B" must be corrected to
  DO-178C (or to IEC 61508 SIL 3 if industry-neutral).

This script scans staged source / doc files for the forbidden tokens
and exits non-zero if any are found. Whitelists the project's own
self-documentation (CLAUDE.md note, this script, the migration
memo) where the tokens appear in *explanatory* context.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

# Tokens that must not appear in new content. Match case-sensitively to
# avoid false positives on unrelated identifiers.
FORBIDDEN_PATTERNS = [
    re.compile(r"\bDO-178B\b"),
    re.compile(r"\bDO178B\b"),
]

# Files where the forbidden token may legitimately appear because they
# are *the* file documenting that the token is forbidden. Add new
# entries with care.
WHITELIST = {
    "CLAUDE.md",
    "scripts/checks/check_obsolete_standards.py",
    "scripts/git/pre-commit",
    "docs/MCDC.md",
}


def staged_files() -> list[str]:
    """Return the list of files staged for the in-progress commit."""
    out = subprocess.run(
        ["git", "diff", "--cached", "--name-only", "--diff-filter=ACMR"],  # noqa: S607  # trusted: git is a fixed dev-tool name
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    return [line for line in out.splitlines() if line]


def scannable(path: Path) -> bool:
    if not path.is_file():
        return False
    if str(path) in WHITELIST:
        return False
    if any(part == "third_party" for part in path.parts):
        return False
    suffix = path.suffix.lower()
    if suffix in {
        ".c",
        ".h",
        ".cpp",
        ".hpp",
        ".md",
        ".py",
        ".sh",
        ".cmake",
        ".yml",
        ".yaml",
        ".txt",
    }:
        return True
    return path.name in {"Makefile", "CMakeLists.txt"}


def main() -> int:
    findings: list[tuple[str, int, str]] = []
    for name in staged_files():
        path = Path(name)
        if not scannable(path):
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        for lineno, line in enumerate(text.splitlines(), start=1):
            for pat in FORBIDDEN_PATTERNS:
                if pat.search(line):
                    findings.append((name, lineno, line.rstrip()))
                    break

    if findings:
        print("[FAIL] Obsolete standard reference detected (DO-178B was")
        print("       superseded by DO-178C in December 2011). Use")
        print("       DO-178C, IEC 61508 SIL 3, or ISO 26262 ASIL C/D")
        print("       per CLAUDE.md. Offending lines:")
        for name, lineno, line in findings:
            print(f"  {name}:{lineno}: {line}")
        return 1

    print("check_obsolete_standards.py: 0 findings.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
