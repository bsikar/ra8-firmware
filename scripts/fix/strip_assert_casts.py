#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Remove redundant integer casts from TEST_ASSERT_EQ arguments.

TEST_ASSERT_EQ internally widens both arguments to int64_t, so any outer
cast like (int), (int32_t), (uint32_t), etc. is redundant and misleading:
a (int) cast applied to a uint32_t enum truncates the value to 32-bit
signed before the macro widens it again.

This script strips those casts from the two argument positions only -- it
does not touch casts inside other function calls.

Limitation: a cast that TRUNCATES an out-of-range value is load-bearing, not
redundant -- e.g. ``(uint16_t)~x``, where the bare ``~x`` promotes to a negative
int. This script cannot tell those apart, so verify the full host suite after a
run and hoist any load-bearing cast into a typed local before the assertion.
"""

import re
import sys
from pathlib import Path

_TYPES = r"u?int(?:8|16|32|64)?_t|int|size_t|ssize_t"
_CAST_RE = re.compile(r"^(\s*)\((?:" + _TYPES + r")\)(.*)", re.DOTALL)

MACRO = "TEST_ASSERT_EQ("


def _strip_leading_cast(text: str) -> str:
    """Remove one leading integer cast from an argument, if present.

    Only the outermost, leading cast: a cast deeper in the expression is
    usually load-bearing and is left alone.
    """
    m = _CAST_RE.match(text)
    return (m.group(1) + m.group(2)) if m else text


def _skip_token(text: str, i: int) -> int:
    """Index just past a literal or comment at ``i``; ``i`` itself for plain code.

    Escapes are handled, so an embedded quote does not end the literal early.

    Bracket/comma scanning must never count a ``(``, ``)`` or ``,`` that lives
    inside a ``"..."`` / ``'...'`` literal or a ``/* */`` / ``//`` comment, or the
    depth accounting drifts and the scan runs off the end of the file.
    """
    c = text[i]
    nxt = text[i + 1] if i + 1 < len(text) else ""
    if c in "\"'":
        j = i + 1
        while j < len(text):
            if text[j] == "\\":
                j += 2
                continue
            if text[j] == c:
                return j + 1
            j += 1
        return j
    if c == "/" and nxt == "/":
        j = text.find("\n", i)
        return len(text) if j == -1 else j
    if c == "/" and nxt == "*":
        j = text.find("*/", i + 2)
        return len(text) if j == -1 else j + 2
    return i


def _find_close_paren(text: str, start: int) -> int:
    """Offset of the ``)`` closing the paren already opened before ``start``.

    Skips literals and comments via ``_skip_token``, so a parenthesis inside a
    string cannot unbalance the depth count.
    """
    depth = 1
    i = start
    while i < len(text) and depth:
        j = _skip_token(text, i)
        if j != i:
            i = j
            continue
        c = text[i]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        i += 1
    return i - 1


def process(content: str) -> str:
    """Strip redundant leading casts from every TEST_ASSERT_EQ in one pass.

    A SINGLE pass: removing a cast can expose another one beneath it, so this
    is not guaranteed to reach a fixed point on its own -- callers should use
    ``process_to_convergence``.
    """
    out = []
    pos = 0
    macro_len = len(MACRO)
    while True:
        idx = content.find(MACRO, pos)
        if idx == -1:
            out.append(content[pos:])
            break

        out.append(content[pos : idx + macro_len])
        inner_start = idx + macro_len
        close = _find_close_paren(content, inner_start)
        inner = content[inner_start:close]

        depth = 0
        split = None
        k = 0
        while k < len(inner):
            j = _skip_token(inner, k)
            if j != k:
                k = j
                continue
            ch = inner[k]
            if ch in "([{":
                depth += 1
            elif ch in ")]}":
                depth -= 1
            elif ch == "," and depth == 0:
                split = k
                break
            k += 1

        if split is None:
            out.append(inner)
            out.append(")")
        else:
            arg1 = _strip_leading_cast(inner[:split])
            arg2 = _strip_leading_cast(inner[split + 1 :])
            out.append(arg1)
            out.append(",")
            out.append(arg2)
            out.append(")")

        pos = close + 1

    return "".join(out)


def process_to_convergence(content: str) -> str:
    """Re-run ``process`` until the text stops changing.

    Bounded to ten iterations rather than looping until stable: a bug that
    made the transform oscillate between two forms would otherwise hang the
    pre-commit hook. In practice one pass fixes everything and a second
    confirms it.
    """
    for _ in range(10):
        next_pass = process(content)
        if next_pass == content:
            break
        content = next_pass
    return content


def main() -> int:
    """Rewrite the files named on argv in place, reporting how many changed.

    Always writes -- there is no dry-run mode here, because check_assert_casts
    is the read-only half of this pair and CI runs that one.

    Returns 1 on the empty-argv usage error, 0 otherwise.
    """
    paths = [Path(p) for p in sys.argv[1:]]
    if not paths:
        print("usage: strip_assert_casts.py <file> [...]", file=sys.stderr)
        return 1
    changed = 0
    for path in paths:
        original = path.read_text(encoding="ascii")
        fixed = process_to_convergence(original)
        if fixed != original:
            path.write_text(fixed, encoding="ascii")
            changed += 1
            print(f"fixed: {path}")
    print(f"{changed}/{len(paths)} file(s) modified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
