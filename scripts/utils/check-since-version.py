#!/usr/bin/env python3
"""
check-since-version.py -- enforce @since Doxygen tags on every
public function declaration in C headers.

Rule: every line that looks like `ra_err_t ra_xxx_yyy(...);` in a
`.h` file under `libs/ra_*/inc/` must be preceded within the last
30 lines by a `@since` tag in a Doxygen block.

Run from pre-commit:
    python3 scripts/utils/check-since-version.py path/to/file.h [more...]
"""

from __future__ import annotations

import pathlib
import re
import sys

PUBLIC_DECL = re.compile(r"""
    ^(?:\[\[nodiscard\]\]\s+)?
    (?:static\s+inline\s+)?
    \s*ra_\w+(?:\s*\*)?\s+
    (ra_\w+)\s*
    \(
""", re.VERBOSE)

SINCE_TAG = re.compile(r"@since")


def check_file(path: pathlib.Path) -> list[str]:
    problems: list[str] = []
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeDecodeError):
        return problems

    for i, line in enumerate(lines):
        match = PUBLIC_DECL.match(line)
        if not match:
            continue
        lookback = "\n".join(lines[max(0, i - 30): i])
        if not SINCE_TAG.search(lookback):
            problems.append(f"{path}:{i + 1}: {match.group(1)} missing @since")
    return problems


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: check-since-version.py FILE [FILE ...]", file=sys.stderr)
        return 2

    failures: list[str] = []
    for raw in sys.argv[1:]:
        path = pathlib.Path(raw)
        if not path.exists() or path.suffix != ".h":
            continue
        if "libs/ra_" not in str(path):
            continue
        failures.extend(check_file(path))

    if failures:
        for line in failures:
            print(line, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
