#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Inclusive-terminology gate for commit message text read from stdin.

Called by the inclusive-terminology CI workflow to verify that no commit
message on a push or PR contains banned legacy SPI/I2C terminology.

Usage:
    git log BASE..HEAD --format=%B | python3 scripts/checks/check_inclusive_terminology_commits.py

Exit code:
  0 -- clean
  1 -- violations found

Per-line opt-out: include LEGACY-OK: <reason> anywhere on the offending line.

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

text = sys.stdin.read()
violations: list[str] = []
for lineno, line in enumerate(text.splitlines(), 1):
    if LEGACY_OK.search(line):
        continue
    for pattern, msg in BANNED:
        if pattern.search(line):
            violations.append(f"  line {lineno}: {msg}\n    > {line.strip()}")
            break

if violations:
    print("[FAIL] Non-inclusive terminology in commit message(s):")
    for v in violations:
        print(v)
    sys.exit(1)

print("[PASS] Commit message terminology clean.")
