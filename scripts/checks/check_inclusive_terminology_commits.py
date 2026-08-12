#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Inclusive-terminology gate for commit message text read from stdin.

Called by the inclusive-terminology CI workflow to verify that no commit
message on a push or PR contains banned legacy SPI/I2C terminology.

Usage:
    git log BASE..HEAD --format=%B | python3 scripts/checks/check_inclusive_terminology_commits.py
    python3 scripts/checks/check_inclusive_terminology_commits.py --selftest

Exit code:
  0 -- clean
  1 -- violations found

Per-paragraph opt-out: include LEGACY-OK: <reason> anywhere in the same
blank-line-delimited paragraph as the offending word. Commit message prose is
word-wrapped by editors and git itself, so an opt-out placed at the end of a
paragraph must cover every physical line of that paragraph, not only the one
it sits on -- a same-line-only rule rejects annotations on ordinary wrapped
text.

@copyright Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import re
import sys

BANNED: tuple[tuple[re.Pattern[str], str], ...] = (
    (re.compile(r"\bmaster(s|ed|ing|ship)?\b", re.IGNORECASE), "master -- use Primary/Controller"),
    (re.compile(r"\bslave(s|d)?\b", re.IGNORECASE), "slave -- use Peripheral"),
    (re.compile(r"\bMOSI\b"), "MOSI -- use COPI"),
    (re.compile(r"\bMISO\b"), "MISO -- use CIPO"),
    (re.compile(r"\bSlave[ _-]Select\b", re.IGNORECASE), "Slave Select -- use CS"),
    # Note: bare '\bSS\b' is intentionally omitted here -- it produces too many
    # false positives in prose commit messages (e.g. "ban SS -> use CS" documenting
    # the rule itself).  The abbreviation is caught at the source-file level by
    # check_inclusive_terminology.py instead.
)

LEGACY_OK: re.Pattern[str] = re.compile(r"LEGACY-OK\s*:", re.IGNORECASE)


def find_violations(text: str) -> list[str]:
    """Scan commit-message text for banned terms, honoring paragraph-scoped opt-outs."""
    violations: list[str] = []
    lines = text.splitlines()
    para_start = 0
    for idx in range(len(lines) + 1):
        at_boundary = idx == len(lines) or lines[idx].strip() == ""
        if not at_boundary:
            continue
        para_lines = lines[para_start:idx]
        if para_lines and not any(LEGACY_OK.search(pl) for pl in para_lines):
            for offset, line in enumerate(para_lines):
                lineno = para_start + offset + 1
                for pattern, msg in BANNED:
                    if pattern.search(line):
                        violations.append(f"  line {lineno}: {msg}\n    > {line.strip()}")
                        break
        para_start = idx + 1
    return violations


def _selftest() -> int:
    """Assert both directions: a must-fire case and a must-stay-quiet case."""
    fired = find_violations("fix(spi): rework the MOSI/MISO pin mux\n")
    if not fired:
        print("[SELFTEST FAIL] an un-annotated MOSI in a commit message was not flagged.")
        return 1

    # A wrapped paragraph: the banned word is on one physical line, the
    # opt-out on another, inside the SAME paragraph. This is exactly the
    # shape ordinary git line-wrapping produces and must stay quiet.
    quiet = find_violations(
        "ci(gates): widen scope\n\n"
        "Widening surfaced only verbatim upstream terms (IEEE 1588 PTP\n"
        "master/slave, datasheet MOSI/MISO pin labels), LEGACY-OK: upstream\n"
        "domain terminology quoted verbatim, not our naming.\n"
    )
    if quiet:
        print("[SELFTEST FAIL] a paragraph-scoped LEGACY-OK opt-out did not cover its")
        print("                whole paragraph:")
        for v in quiet:
            print(v)
        return 1

    # A LEGACY-OK in a DIFFERENT paragraph must NOT reach across the blank
    # line -- the opt-out is paragraph-scoped, not whole-message-scoped.
    cross_paragraph = find_violations(
        "fix(spi): rework the MOSI pin mux\n\nLEGACY-OK: unrelated note in the next paragraph\n"
    )
    if not cross_paragraph:
        print("[SELFTEST FAIL] LEGACY-OK in one paragraph suppressed a violation in a")
        print("                different paragraph.")
        return 1

    print("[SELFTEST OK] fires on an un-annotated term, stays quiet on a wrapped")
    print("              paragraph-scoped LEGACY-OK, and does not leak across paragraphs.")
    return 0


def main() -> int:
    """Run --selftest, or scan stdin for banned terms and report."""
    if "--selftest" in sys.argv[1:]:
        return _selftest()

    text = sys.stdin.read()
    violations = find_violations(text)

    if violations:
        print("[FAIL] Non-inclusive terminology in commit message(s):")
        for v in violations:
            print(v)
        return 1

    print("[PASS] Commit message terminology clean.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
