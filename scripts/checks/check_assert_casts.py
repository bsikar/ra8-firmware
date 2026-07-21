#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: reject explicit integer casts inside TEST_ASSERT_EQ arguments.

TEST_ASSERT_EQ widens both arguments to int64_t internally.  An outer
cast like (int) or (int32_t) is therefore redundant -- and a (int) cast
applied to a uint32_t enum silently truncates the value to 32-bit signed
before the widening, which can produce a false-passing comparison for
values >= 0x80000000.

Run: check_assert_casts.py <file> [...]
Exit 0 if clean, 1 if any violation found.
"""

import re
import sys
from pathlib import Path

_TYPES = r"u?int(?:8|16|32|64)?_t|int|size_t|ssize_t"
_CAST_RE = re.compile(r"\((?:" + _TYPES + r")\)")
_MACRO = "TEST_ASSERT_EQ("


def _find_close_paren(text: str, start: int) -> int:
    depth = 1
    i = start
    while i < len(text) and depth:
        c = text[i]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        i += 1
    return i - 1


def _has_leading_cast(text: str) -> bool:
    return bool(re.match(r"^\s*\((?:" + _TYPES + r")\)", text))


def check(path: Path) -> list[str]:
    content = path.read_text(encoding="ascii", errors="replace")
    violations = []
    macro_len = len(_MACRO)
    pos = 0
    while True:
        idx = content.find(_MACRO, pos)
        if idx == -1:
            break
        inner_start = idx + macro_len
        close = _find_close_paren(content, inner_start)
        inner = content[inner_start:close]

        depth = 0
        split = None
        for i, ch in enumerate(inner):
            if ch in "([{":
                depth += 1
            elif ch in ")]}":
                depth -= 1
            elif ch == "," and depth == 0:
                split = i
                break

        line_no = content[:idx].count("\n") + 1
        if split is None:
            pos = close + 1
            continue

        arg1 = inner[:split]
        arg2 = inner[split + 1 :]
        if _has_leading_cast(arg1):
            violations.append(
                f"{path}:{line_no}: cast in first arg of TEST_ASSERT_EQ: "
                f"{_MACRO}{arg1.strip()[:60]}..."
            )
        if _has_leading_cast(arg2):
            violations.append(
                f"{path}:{line_no}: cast in second arg of TEST_ASSERT_EQ: ...{arg2.strip()[:60]}"
            )
        pos = close + 1
    return violations


def main() -> int:
    paths = [Path(p) for p in sys.argv[1:]]
    if not paths:
        print("usage: check_assert_casts.py <file> [...]", file=sys.stderr)
        return 1
    all_violations: list[str] = []
    for p in paths:
        all_violations.extend(check(p))
    for v in all_violations:
        print(v)
    if all_violations:
        print(
            f"\n{len(all_violations)} redundant cast(s) in TEST_ASSERT_EQ.\n"
            "Run scripts/fix/strip_assert_casts.py to fix automatically.",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
