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
import tempfile
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
    """Report TEST_ASSERT_EQ arguments that OPEN with a redundant integer cast.

    Only a cast in leading position counts, on either argument. A cast deeper
    in the expression is left alone deliberately -- there it is usually load
    bearing (narrowing a wider intermediate), whereas an outer one is applied
    to a value the macro is about to widen to int64_t anyway.

    The macro's two arguments are split at the first comma seen at paren depth
    zero, so a comma inside a nested call or a compound literal does not shift
    the second argument. An invocation with no such comma is skipped rather
    than reported: that is a syntax error, and the compiler names it better.

    Returns one preformatted ``path:line: message`` per violation, in file
    order; an empty list means clean.
    """
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


def selftest() -> int:
    """Prove leading casts fire while clean and nested casts stay quiet."""
    with tempfile.TemporaryDirectory(prefix="assert-casts-selftest-") as raw:
        root = Path(raw)
        bad = root / "bad.c"
        good = root / "good.c"
        bad.write_text(
            "TEST_ASSERT_EQ((int)value, (uint32_t)expected);\n",
            encoding="ascii",
        )
        good.write_text(
            "TEST_ASSERT_EQ(value, expected);\nTEST_ASSERT_EQ(load((int)value), expected);\n",
            encoding="ascii",
        )
        bad_findings = check(bad)
        good_findings = check(good)
    expected_bad_findings = 2
    cases = (
        (len(bad_findings) == expected_bad_findings, "leading casts on both arguments fire"),
        (not good_findings, "clean and nested casts stay quiet"),
    )
    failed = [label for passed, label in cases if not passed]
    for passed, label in cases:
        print(f"  [{'ok' if passed else 'FAIL'}] {label}")
    if failed:
        print(f"check_assert_casts.py --selftest: {len(failed)} failure(s)", file=sys.stderr)
        return 1
    print("check_assert_casts.py --selftest: all cases pass (both directions).")
    return 0


def main() -> int:
    """Scan the files named on argv and print every finding to stdout."""
    args = sys.argv[1:]
    if args == ["--selftest"]:
        return selftest()
    if any(arg.startswith("-") and arg != "--all" for arg in args) or (
        "--all" in args and args != ["--all"]
    ):
        print("check_assert_casts.py: unknown or incompatible arguments", file=sys.stderr)
        return 2
    if args == ["--all"]:
        repo_root = Path(__file__).resolve().parents[2]
        paths = sorted((repo_root / "tests").rglob("*.c"))
    else:
        paths = [Path(p) for p in args]
    if not paths:
        print(
            "usage: check_assert_casts.py <file> [...] or check_assert_casts.py --all",
            file=sys.stderr,
        )
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
