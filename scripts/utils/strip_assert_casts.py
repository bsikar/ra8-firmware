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
"""

import re
import sys
from pathlib import Path

_TYPES = r'u?int(?:8|16|32|64)?_t|int|size_t|ssize_t'
_CAST_RE = re.compile(r'^(\s*)\((?:' + _TYPES + r')\)(.*)', re.DOTALL)

MACRO = 'TEST_ASSERT_EQ('


def _strip_leading_cast(text: str) -> str:
    m = _CAST_RE.match(text)
    return (m.group(1) + m.group(2)) if m else text


def _find_close_paren(text: str, start: int) -> int:
    depth = 1
    i = start
    while i < len(text) and depth:
        c = text[i]
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
        i += 1
    return i - 1


def process(content: str) -> str:
    out = []
    pos = 0
    macro_len = len(MACRO)
    while True:
        idx = content.find(MACRO, pos)
        if idx == -1:
            out.append(content[pos:])
            break

        out.append(content[pos:idx + macro_len])
        inner_start = idx + macro_len
        close = _find_close_paren(content, inner_start)
        inner = content[inner_start:close]

        depth = 0
        split = None
        for i, ch in enumerate(inner):
            if ch in '([{':
                depth += 1
            elif ch in ')]}':
                depth -= 1
            elif ch == ',' and depth == 0:
                split = i
                break

        if split is None:
            out.append(inner)
            out.append(')')
        else:
            arg1 = _strip_leading_cast(inner[:split])
            arg2 = _strip_leading_cast(inner[split + 1:])
            out.append(arg1)
            out.append(',')
            out.append(arg2)
            out.append(')')

        pos = close + 1

    return ''.join(out)


def process_to_convergence(content: str) -> str:
    for _ in range(10):
        next_pass = process(content)
        if next_pass == content:
            break
        content = next_pass
    return content


def main() -> int:
    paths = [Path(p) for p in sys.argv[1:]]
    if not paths:
        print('usage: strip_assert_casts.py <file> [...]', file=sys.stderr)
        return 1
    changed = 0
    for path in paths:
        original = path.read_text(encoding='ascii')
        fixed = process_to_convergence(original)
        if fixed != original:
            path.write_text(fixed, encoding='ascii')
            changed += 1
            print(f'fixed: {path}')
    print(f'{changed}/{len(paths)} file(s) modified')
    return 0


if __name__ == '__main__':
    sys.exit(main())
