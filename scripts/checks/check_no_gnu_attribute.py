#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""check_no_gnu_attribute.py -- enforce the C23 [[...]] attribute syntax.

Per CLAUDE.md, first-party code uses the standard C23 attribute-specifier
form [[...]] (e.g. [[gnu::weak]], [[noreturn]], [[maybe_unused]],
[[nodiscard]]) instead of the GNU __attribute__((...)) form.

  void f(void) __attribute__((weak));     # REJECTED
  [[gnu::weak]] void f(void);             # OK

Scope:
  C / C++ sources (.c .h .cpp .hpp) under libs/, src/, tests/, examples/,
  port/, tools/.  Vendored SOUP (libs/third_party/*) and generated font
  data (libs/fonts/*) are exempt, as are build trees.

Allowed __attribute__ uses (no portable [[...]] spelling -- clang errors on
the [[gnu::]] form as an unknown attribute while it silently ignores the GNU
form, so these MUST stay __attribute__):
  * interrupt
  * cmse_nonsecure_entry
  * cmse_nonsecure_call
(with or without the __leading_trailing__ underscores).

Other exemptions:
  * Comment lines / prose mentioning __attribute__ (lines whose attribute
    token sits inside a // or /* comment).
  * Any line carrying `ATTR-OK: <reason>` (reason text required).
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from lint_targets import is_build_output_path

ROOTS = ("libs", "src", "tests", "examples", "port", "tools")
EXTS = (".c", ".h", ".cpp", ".hpp")
EXEMPT_DIRS = ("/third_party/", "/fonts/")
ALLOWED = {"interrupt", "cmse_nonsecure_entry", "cmse_nonsecure_call"}

ATTR_RE = re.compile(r"__attribute__\s*\(\(")
WAIVER_RE = re.compile(r"ATTR-OK:\s*\S")

# Length of a bare `____` wrapper (two leading + two trailing underscores); a
# dunder name must exceed it to carry a body worth stripping.
_MIN_DUNDER_LEN = 4


def _strip_us(name: str) -> str:
    """Strip the surrounding double underscores from an attribute name.

    ``__packed__`` and ``packed`` are the same attribute to GCC, so both
    spellings must normalise to one before comparison or half of them would
    slip past.
    """
    if len(name) > _MIN_DUNDER_LEN and name.startswith("__") and name.endswith("__"):
        return name[2:-2]
    return name


def _attr_body(line: str, pos: int) -> str | None:
    """Balanced-paren body of the __attribute__ starting at/after pos."""
    p = line.find("((", pos)
    if p < 0:
        return None
    p += 2
    depth, q = 1, p
    while q < len(line):
        if line[q] == "(":
            depth += 1
        elif line[q] == ")":
            depth -= 1
            if depth == 0:
                return line[p:q]
        q += 1
    return None


def _is_comment_pos(line: str, pos: int) -> bool:
    """True if column `pos` sits inside a // or /* comment on this line."""
    stripped = line.lstrip()
    if stripped.startswith(("*", "//", "/*")):
        return True
    before = line[:pos]
    if "//" in before:
        return True
    # crude single-line /* ... */ check
    return "/*" in before and "*/" not in before


def discover() -> list[str]:
    """Every first-party source file, for the whole-tree sweep."""
    out: list[str] = []
    for root in ROOTS:
        base = Path(root)
        if not base.is_dir():
            continue
        for ext in EXTS:
            for f in base.rglob(f"*{ext}"):
                s = str(f)
                if is_build_output_path(s) or any(d in s for d in EXEMPT_DIRS):
                    continue
                out.append(s)
    return out


def check_file(path: str) -> list[tuple[int, str]]:
    """Report every legacy ``__attribute__((...))`` in one file.

    The tree uses the C23 ``[[...]]`` form. Note the two spellings that CANNOT
    migrate and are excluded rather than reported -- ``interrupt`` and the
    CMSE ``cmse_nonsecure_entry`` -- neither of which has a standard-attribute
    equivalent the toolchain accepts.
    """
    findings: list[tuple[int, str]] = []
    try:
        text = Path(path).read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        return findings
    if "__attribute__" not in text:
        return findings
    for i, line in enumerate(text.splitlines(), 1):
        for m in ATTR_RE.finditer(line):
            if _is_comment_pos(line, m.start()):
                continue
            if WAIVER_RE.search(line):
                continue
            body = _attr_body(line, m.start())
            names = {
                _strip_us(t.strip().split("(")[0].strip())
                for t in (body.split(",") if body else [])
            }
            if names and names <= ALLOWED:
                continue
            findings.append((i, line.strip()[:100]))
    return findings


def main() -> int:
    """Fail on GNU ``__attribute__`` syntax where the C23 form is available.

    Position matters for the C23 form -- it must precede the declaration
    specifiers rather than follow them -- which is why this is a migration
    worth finishing rather than a style preference: the two spellings do not
    sit in the same place, so mixed usage reads inconsistently.

    Returns 1 listing each occurrence, 0 when clean.
    """
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    files = args or discover()
    total = 0
    for path in sorted(set(files)):
        if (
            not path.endswith(EXTS)
            or is_build_output_path(path)
            or any(d in path for d in EXEMPT_DIRS)
        ):
            continue
        for ln, snippet in check_file(path):
            print(
                f"{path}:{ln}: GNU __attribute__ -- use the C23 [[...]] form "
                f"(e.g. [[gnu::weak]]); {snippet}"
            )
            total += 1
    if total:
        print(
            f"\ncheck_no_gnu_attribute: {total} violation(s). Migrate to [[...]] "
            f"(only interrupt / cmse_nonsecure_entry / cmse_nonsecure_call may stay "
            f"__attribute__; add `ATTR-OK: <reason>` for a justified exception)."
        )
        return 1
    print("check_no_gnu_attribute: clean -- all attributes use the C23 [[...]] form.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
