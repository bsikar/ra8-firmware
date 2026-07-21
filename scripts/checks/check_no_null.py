#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""check_no_null.py -- enforce the C23 nullptr-only rule on first-party code.

Project policy: production code (libs/, src/, port/, examples/<app>/) must
use ``nullptr`` instead of ``NULL`` for null pointer constants. This script
walks staged or all-tracked source files and rejects bare ``NULL`` tokens
in code positions. It allows NULL in:

  * comment lines and inline `/* ... */` / `// ...` comments
  * string literals
  * Doxygen annotation prose
  * UX_NULL / similar vendor macros (USBX expects literal NULL)

Vendored trees under libs/third_party/ are skipped wholesale.

Usage:
    python3 scripts/checks/check_no_null.py FILE [FILE ...]
    python3 scripts/checks/check_no_null.py --all

Returns 0 on clean, 1 on findings, 2 on usage error.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
from collections.abc import Iterable

# Files we author. Vendored / generated trees are skipped.
ROOT_DIRS = ("libs", "src", "port", "examples")

EXCLUDED_PARTS = {"third_party", "_deps", "build", "build-cov"}
EXTENSIONS = {".c", ".h", ".cpp", ".hpp"}

# bare NULL token in a code context. \bNULL\b matches the identifier;
# we strip comments and strings first so this only fires in code.
NULL_RE = re.compile(r"\bNULL\b")


# Strip C/C++ inline comments and string/char literals so we don't
# match NULL inside them. Naive but adequate for this use case.
def _strip_noncode(line: str) -> str:  # noqa: PLR0912 PLR0915  # char-by-char state machine, splitting hurts readability
    out = []
    i = 0
    in_str = False
    in_chr = False
    in_lc = False
    in_bc = False
    n = len(line)
    while i < n:
        c = line[i]
        nxt = line[i + 1] if i + 1 < n else ""
        if in_lc:
            break
        if in_bc:
            if c == "*" and nxt == "/":
                in_bc = False
                i += 2
                continue
            i += 1
            continue
        if in_str:
            if c == "\\" and i + 1 < n:
                i += 2
                continue
            if c == '"':
                in_str = False
            i += 1
            continue
        if in_chr:
            if c == "\\" and i + 1 < n:
                i += 2
                continue
            if c == "'":
                in_chr = False
            i += 1
            continue
        if c == "/" and nxt == "/":
            break
        if c == "/" and nxt == "*":
            in_bc = True
            i += 2
            continue
        if c == '"':
            in_str = True
            i += 1
            continue
        if c == "'":
            in_chr = True
            i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


# Identifiers that LOOK like NULL but are actually allowed: vendor macros
# the project doesn't own (USBX expects literal NULL in its API contract,
# ThreadX-FileX-NetXDuo similar). We accept a token if its full
# identifier is in this set or it has one of these suffixes.
ALLOWED_TOKENS = {"UX_NULL", "TX_NULL", "FX_NULL", "NX_NULL"}


def find_violations(path: pathlib.Path) -> list[tuple[int, str]]:
    violations: list[tuple[int, str]] = []
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return violations
    in_block_comment = False
    for n, raw in enumerate(text.splitlines(), 1):
        # Multi-line block comment continuation handling.
        cur = raw
        if in_block_comment:
            end = cur.find("*/")
            if end == -1:
                continue
            cur = cur[end + 2 :]
            in_block_comment = False
        # Detect a /* on this line that doesn't close.
        bo = cur.find("/*")
        if bo != -1 and cur.find("*/", bo + 2) == -1:
            cur = cur[:bo]
            in_block_comment = True
        code = _strip_noncode(cur)
        if "NULL" not in code:
            continue
        # Skip allowed tokens.
        # Replace allowed tokens with a placeholder so the regex doesn't fire.
        scrubbed = code
        for tok in ALLOWED_TOKENS:
            scrubbed = scrubbed.replace(tok, "_OK_")
        if NULL_RE.search(scrubbed):
            violations.append((n, raw.strip()))
    return violations


def needs_check(path: pathlib.Path) -> bool:
    if path.suffix.lower() not in EXTENSIONS:
        return False
    return not any(part in EXCLUDED_PARTS for part in path.parts)


def iter_all_files() -> Iterable[pathlib.Path]:
    for root in ROOT_DIRS:
        rp = pathlib.Path(root)
        if not rp.is_dir():
            continue
        for p in rp.rglob("*"):
            if p.is_file() and needs_check(p):
                yield p


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--all", action="store_true", help="scan all tracked source files")
    parser.add_argument(
        "files", nargs="*", type=pathlib.Path, help="explicit file list (e.g. staged files)"
    )
    args = parser.parse_args()

    if args.all:
        candidates = list(iter_all_files())
    elif args.files:
        candidates = [p for p in args.files if needs_check(p)]
    else:
        parser.print_usage(sys.stderr)
        return 2

    total_violations = 0
    for path in candidates:
        for line, snippet in find_violations(path):
            print(f"{path}:{line}: bare NULL -- use nullptr (C23): {snippet}", file=sys.stderr)
            total_violations += 1

    if total_violations:
        print(
            f"\n{total_violations} bare NULL token(s) found. Replace with "
            "`nullptr` (C23 builtin). Allowed: UX_NULL / TX_NULL / FX_NULL "
            "/ NX_NULL vendor macros, comments, string literals.",
            file=sys.stderr,
        )
        return 1
    print("check_no_null.py: 0 findings.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
