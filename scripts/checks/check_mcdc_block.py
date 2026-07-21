#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
r"""check_mcdc_block.py -- Require @par MC/DC blocks on tests of compound decisions.

Per CLAUDE.md "IEC 61508 SIL 3 / DO-178C Level B Qualification":

  Tests must declare their MC/DC vector pattern via @par MC/DC: in
  Doxygen.

This script enforces the rule on staged test files. For any staged
file under tests/test_*.c that adds a new TEST(...) / TEST_F(...) /
test_*(void) function (heuristic: presence of `TEST(` or function
definitions matching `^[a-z_].*test.*\\(void\\)`), we require the
function's preceding Doxygen block to contain `@par MC/DC:`.

In this is a WARNING. Once the existing 1956-vector gap from
docs/MCDC_GAPS.csv is closed, the hook will be promoted to STRICT
(non-zero exit on any missing block).
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

WARN_ONLY_MODE = False  # Legacy 138-test backlog cleared (2026-05-02); STRICT enforcement enabled.

MAX_DISPLAYED_FINDINGS = 50  # Max number of findings to print before summarizing the rest.

TEST_FUNC_PATTERN = re.compile(
    r"""
    (?:
        ^\s*TEST(?:_F)?\s*\(\s*([A-Za-z_]\w*)\s*,\s*([A-Za-z_]\w*)\s*\)
        |
        ^\s*(?:static\s+)?(?:void|int|UINT)\s+(test_[A-Za-z_]\w*)\s*\(\s*void\s*\)
    )
    """,
    re.VERBOSE | re.MULTILINE,
)

DOXYGEN_BLOCK_END_RE = re.compile(r"\*/\s*$")
MCDC_TAG_RE = re.compile(r"@par\s+MC/DC\s*:", re.IGNORECASE)


def staged_test_files() -> list[Path]:
    """Staged test files, excluding deletions.

    Scoped to the index because this runs as a pre-commit hook: it polices
    what is about to be committed, not the tests already in the tree.
    """
    out = subprocess.run(
        ["git", "diff", "--cached", "--name-only", "--diff-filter=ACMR"],  # noqa: S607  # trusted: fixed git argv
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    return [
        Path(p)
        for p in out.splitlines()
        if p.startswith("tests/") and p.endswith(".c") and Path(p).is_file()
    ]


def preceding_doxygen_block(lines: list[str], func_lineno: int) -> str:
    """Text of the doxygen block immediately above a 1-based function line.

    Blank lines between the block and the function are tolerated, so ordinary
    spacing does not detach a block from what it documents.

    Returns "" when no block precedes the function.
    """
    end = func_lineno - 2  # zero-based index of line just above the function
    while end >= 0 and not lines[end].strip():
        end -= 1
    if end < 0:
        return ""
    if not DOXYGEN_BLOCK_END_RE.search(lines[end]):
        return ""
    start = end
    while start >= 0 and "/**" not in lines[start]:
        start -= 1
    if start < 0:
        return ""
    return "\n".join(lines[start : end + 1])


def main() -> int:
    """Require a ``@par MC/DC:`` block on every newly-staged test of a compound decision.

    The block is where a test states its vector pattern and its independence
    argument. Without it a test can exercise a compound decision and still
    prove nothing about MC/DC -- it looks like coverage, and the distinction
    is exactly what DO-178C Level B turns on.

    Returns 1 listing each test missing the block, 0 when the staged set is
    clean.
    """
    findings: list[tuple[str, int, str]] = []
    for path in staged_test_files():
        text = path.read_text(encoding="utf-8", errors="ignore")
        lines = text.splitlines()
        for match in TEST_FUNC_PATTERN.finditer(text):
            func_name = next((g for g in match.groups() if g), "<unknown>")
            func_line = text[: match.start()].count("\n") + 1
            block = preceding_doxygen_block(lines, func_line)
            if not block or not MCDC_TAG_RE.search(block):
                findings.append((str(path), func_line, func_name))

    if findings:
        verb = "WARN" if WARN_ONLY_MODE else "FAIL"
        print(f"[{verb}] Tests missing required @par MC/DC: block")
        print("       Per CLAUDE.md, every test of a compound boolean")
        print("       decision must document its MC/DC vector pattern.")
        for name, lineno, func in findings[:MAX_DISPLAYED_FINDINGS]:
            print(f"  {name}:{lineno}: {func}")
        if len(findings) > MAX_DISPLAYED_FINDINGS:
            print(f"  ... and {len(findings) - MAX_DISPLAYED_FINDINGS} more")
        if WARN_ONLY_MODE:
            return 0
        return 1

    print("check_mcdc_block.py: 0 findings.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
