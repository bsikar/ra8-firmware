#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""check_c23_patterns.py -- enforce four C23 source patterns on first-party code.

These four rules previously lived ONLY as inline ``grep`` loops inside the
local ``scripts/git/pre-commit`` hook and were never run by the CI gate
``pre-commit-checks`` (``gate_pre_commit_checks`` in
``scripts/ci/gates/checks.sh``).  The workflow claimed the gate mirrored the
hook, but these four checks were absent from it -- a "local green, CI red"
drift in the other direction, where a violation the hook rejects sails through
CI on a machine whose hook is not installed.  This checker is the single
first-party implementation both the hook and the gate now call, so there is
exactly one definition of each rule.

The rules (CLAUDE.md "C23 Syntax" and "Constants and Macros"):

  1. ``_Static_assert(`` -- C11 spelling; C23 provides ``static_assert``.
  2. ``= {0}`` -- C11 zero-initializer; C23 uses ``= {}``.
  3. ``#include <stdbool.h>`` -- unnecessary; ``bool`` is a C23 keyword.
  4. Object-like ``#define NAME <bare-numeric-literal>`` -- the value must be
     paren-wrapped (``#define NAME (1000)``) so it stays a single token in
     every expression context.  Function-like macros, bare feature flags, and
     already-parenthesised values are ignored; hex / binary / float / suffix
     literal forms are all recognised.

Scope:
  C / C++ sources (.c .h .cpp .hpp) under libs/, src/, examples/, port/,
  tools/, tests/.  Vendored SOUP (libs/third_party/) and generated font data
  (libs/ra8_fonts/) are exempt, as are build trees.

Matches inside comments and string / character literals are ignored: the
comment/string blanking is reused verbatim from ``check_magic_numbers.py`` so
the two gates agree byte-for-byte on what "code" is.

Usage:
    python3 scripts/checks/check_c23_patterns.py FILE [FILE ...]
    python3 scripts/checks/check_c23_patterns.py            # staged files
    python3 scripts/checks/check_c23_patterns.py --all      # every tracked file
    python3 scripts/checks/check_c23_patterns.py --selftest # prove the rules fire

Returns 0 on clean, 1 on findings, 2 on usage / selftest failure.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from check_magic_numbers import _strip_comments_and_strings
from lint_targets import is_build_output_path

REPO_ROOT = Path(__file__).resolve().parents[2]

# First-party roots that carry hand-authored C. Mirrors check_no_gnu_attribute.
ROOTS = ("libs", "examples", "port", "tools", "apps", "tests")
EXTS = (".c", ".h", ".cpp", ".hpp")
# Path fragments that exclude a file: vendored SOUP and generated font tables.
EXEMPT_DIRS = ("/third_party/", "/ra8_fonts/")

# Rule 1: C11 _Static_assert at the start of a line (after leading whitespace).
# C23 spells it `static_assert`.
_STATIC_ASSERT_RE = re.compile(r"^\s*_Static_assert\s*\(")

# Rule 2: the C11 `= {0}` zero-initializer. C23 uses `= {}`.
_ZERO_INIT_RE = re.compile(r"= \{0\}")

# Rule 3: `#include <stdbool.h>` -- unnecessary, `bool` is a C23 keyword.
_STDBOOL_RE = re.compile(r"^\s*#\s*include\s+<stdbool\.h>")

# Rule 4: object-like `#define NAME <bare-numeric-literal>` whose value is not
# paren-wrapped. Faithful translation of the ERE in scripts/git/pre-commit:
# a bare integer or float literal (with optional U/L/F suffix, and hex / binary
# / exponent forms), ignoring function-like macros, bare feature flags, and
# already-parenthesised values. The trailing comment group is retained for
# fidelity; the blanking below turns any real comment to spaces, which the
# leading `\s*` absorbs.
_BARE_DEFINE_RE = re.compile(
    r"^\s*#\s*define\s+[A-Za-z_][A-Za-z0-9_]*\s+"
    r"[+-]?(?:"
    r"0[xX][0-9a-fA-F]+[uUlL]*"
    r"|0[bB][01]+[uUlL]*"
    r"|[0-9]+\.[0-9]*(?:[eE][+-]?[0-9]+)?[fFlL]?"
    r"|\.[0-9]+(?:[eE][+-]?[0-9]+)?[fFlL]?"
    r"|[0-9]+(?:[eE][+-]?[0-9]+)?[fFlLuU]*"
    r")\s*(?:/[/*].*)?$"
)

# Each rule: (id, compiled regex, human-facing message). The id doubles as the
# selftest key so a rule cannot be added without a fixture proving both
# directions.
_RULES: tuple[tuple[str, re.Pattern[str], str], ...] = (
    ("static_assert", _STATIC_ASSERT_RE, "C11 _Static_assert -- use C23 static_assert"),
    ("zero_init", _ZERO_INIT_RE, "C11 = {0} initializer -- use C23 = {}"),
    ("stdbool", _STDBOOL_RE, "unnecessary #include <stdbool.h> -- bool is a C23 keyword"),
    ("bare_define", _BARE_DEFINE_RE, "bare numeric #define value -- wrap with parens, e.g. (1000)"),
)


def find_violations(path: Path) -> list[tuple[int, str, str]]:
    """Report every C23-pattern violation in one file.

    The scan runs over a comment/string-blanked view of the source so a match
    inside a comment or a string literal is never reported; line numbers stay
    valid because the blanking preserves newlines and column positions.

    Args:
        path: File to scan.

    Returns:
        A list of ``(line_no, rule_id, snippet)`` tuples, one per finding;
        an unreadable file yields an empty list rather than raising.
    """
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    code_lines = _strip_comments_and_strings(text).splitlines()
    orig_lines = text.splitlines()
    violations: list[tuple[int, str, str]] = []
    for idx, code in enumerate(code_lines):
        for rule_id, pattern, _msg in _RULES:
            if pattern.search(code):
                snippet = (orig_lines[idx] if idx < len(orig_lines) else code).strip()
                violations.append((idx + 1, rule_id, snippet))
    return violations


def needs_check(path: Path) -> bool:
    """Whether a path is first-party C/C++ subject to the C23 pattern rules.

    Args:
        path: Candidate file path.

    Returns:
        True when the suffix is a C/C++ one and the path is neither a build
        artifact nor under a vendored / generated tree.
    """
    if path.suffix.lower() not in EXTS:
        return False
    posix = path.as_posix()
    if is_build_output_path(posix):
        return False
    return not any(frag in f"/{posix}/" for frag in EXEMPT_DIRS)


def _git_lines(*pathspec: str) -> list[str]:
    """Return tracked repo-relative paths matching `pathspec`.

    Args:
        pathspec: git pathspec arguments (e.g. ``"*.c"``).

    Returns:
        Repo-relative path strings; exits 2 on a git failure.
    """
    proc = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool path
        ["git", "ls-files", "-z", "--", *pathspec],  # noqa: S607 -- trusted: fixed git argv
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr)
        sys.stderr.write(f"git ls-files failed (exit {proc.returncode})\n")
        sys.exit(2)
    return [p for p in proc.stdout.split("\0") if p]


def iter_all_files() -> list[Path]:
    """Every tracked first-party C/C++ file, for the ``--all`` sweep.

    Returns:
        Absolute paths under the first-party roots that pass ``needs_check``.
    """
    out: list[Path] = []
    for root in ROOTS:
        for rel in _git_lines(*(f"{root}/**/*{ext}" for ext in EXTS)):
            p = REPO_ROOT / rel
            if needs_check(p):
                out.append(p)
    return sorted(set(out))


def iter_staged_files() -> list[Path]:
    """Every staged first-party C/C++ file, for the default (hook) mode.

    Returns:
        Absolute paths of added / copied / modified / renamed staged files
        that pass ``needs_check`` and still exist on disk.
    """
    proc = subprocess.run(
        ["git", "diff", "--cached", "--name-only", "--diff-filter=ACMR", "-z"],  # noqa: S607 -- trusted git
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr)
        sys.stderr.write(f"git diff --cached failed (exit {proc.returncode})\n")
        sys.exit(2)
    out: list[Path] = []
    for rel in proc.stdout.split("\0"):
        if not rel:
            continue
        p = REPO_ROOT / rel
        if needs_check(p) and p.is_file():
            out.append(p)
    return out


# ---------------------------------------------------------------------------
# Selftest
#
# Every rule is asserted in BOTH directions: a bad fixture must fire exactly
# that rule, and the matching correct-C23 form must stay silent. A comment and
# a string-literal fixture prove the blanking suppresses matches that are not
# really code. Fixtures are scanned in-memory via a temp file, so nothing a
# checker's own tree scan could later trip over is written into the repo.
# ---------------------------------------------------------------------------

# (rule_id, source, should_fire). ``should_fire`` names the rule the source is
# expected to trip; the negative cases assert a clean scan of the whole file.
_SELFTEST_CASES: tuple[tuple[str, str, bool], ...] = (
    # Rule 1: _Static_assert.
    ("static_assert", '_Static_assert(sizeof(int) == 4, "width");\n', True),
    ("static_assert", '  _Static_assert(1, "x");\n', True),
    ("static_assert", 'static_assert(sizeof(int) == 4, "width");\n', False),
    # Rule 2: = {0}.
    ("zero_init", "int table[4] = {0};\n", True),
    ("zero_init", "int table[4] = {};\n", False),
    # Rule 3: #include <stdbool.h>.
    ("stdbool", "#include <stdbool.h>\n", True),
    ("stdbool", "#  include <stdbool.h>\n", True),
    ("stdbool", "#include <stdint.h>\n", False),
    # Rule 4: bare numeric #define.
    ("bare_define", "#define K_TIMEOUT_MS 1000\n", True),
    ("bare_define", "#define K_MASK 0xFF\n", True),
    ("bare_define", "#define K_FLAGS 0b1010u\n", True),
    ("bare_define", "#define K_SCALE 1.5f\n", True),
    ("bare_define", "#define K_TIMEOUT_MS (1000)\n", False),
    ("bare_define", "#define K_TIMEOUT_MS 1000  // trailing comment ok\n", True),
    ("bare_define", "#define RA8_HAS_MVE\n", False),
    ("bare_define", "#define MAX(a, b) ((a) > (b) ? (a) : (b))\n", False),
    # Comment / string suppression: no rule may fire on non-code text.
    ("_clean", "/* _Static_assert(x); int a[1] = {0}; #define K 5 */\n", False),
    ("_clean", 'const char* s = "= {0}";\n', False),
    ("_clean", "// #include <stdbool.h>\n", False),
)


def selftest(tmp: Path) -> int:
    """Prove each rule fires on a bad fixture and stays silent on the C23 form.

    Args:
        tmp: Writable scratch directory for the fixture files.

    Returns:
        0 when every case matches its expectation, 1 otherwise.
    """
    failures: list[str] = []
    for i, (expect_id, source, should_fire) in enumerate(_SELFTEST_CASES):
        fixture = tmp / f"case_{i}.c"
        fixture.write_text(source, encoding="utf-8")
        fired = {rule_id for _line, rule_id, _snip in find_violations(fixture)}
        if should_fire:
            if expect_id not in fired:
                failures.append(f"  case {i}: rule '{expect_id}' did not fire on: {source!r}")
        elif fired:
            failures.append(
                f"  case {i}: rules {sorted(fired)} fired but none expected: {source!r}"
            )
    if failures:
        print("check_c23_patterns.py --selftest: FAILED\n", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        return 1
    fires = sum(1 for c in _SELFTEST_CASES if c[2])
    total = len(_SELFTEST_CASES)
    print(
        f"check_c23_patterns.py --selftest: PASS "
        f"({total} cases: {fires} must fire, {total - fires} must stay silent)"
    )
    return 0


def _resolve_targets(args: argparse.Namespace) -> list[Path]:
    """Decide which files to scan from the parsed arguments.

    Args:
        args: Parsed command-line arguments.

    Returns:
        The list of files to scan, filtered through ``needs_check``.
    """
    if args.all:
        return iter_all_files()
    if args.files:
        return [Path(p) for p in args.files if needs_check(Path(p)) and Path(p).is_file()]
    return iter_staged_files()


def main(argv: list[str]) -> int:
    """Scan first-party C/C++ for the four C23 patterns, or run the selftest.

    With no ``--all`` and no explicit files the staged set is scanned, which is
    how the pre-commit hook stays fast; ``--all`` sweeps every tracked file,
    which is how the CI gate covers the whole tree.

    Args:
        argv: Full argument vector (``sys.argv``).

    Returns:
        0 when clean, 1 on findings or a failing selftest, 2 on a usage error.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--all", action="store_true", help="scan all tracked C/C++ files")
    parser.add_argument(
        "--selftest", action="store_true", help="prove each rule fires and stays silent correctly"
    )
    parser.add_argument("files", nargs="*", help="explicit file list (e.g. staged files)")
    args = parser.parse_args(argv[1:])

    if args.selftest:
        with tempfile.TemporaryDirectory() as td:
            return selftest(Path(td))

    targets = _resolve_targets(args)

    messages = {rule_id: msg for rule_id, _pat, msg in _RULES}
    total = 0
    for path in targets:
        rel = path.relative_to(REPO_ROOT) if path.is_relative_to(REPO_ROOT) else path
        for line_no, rule_id, snippet in find_violations(path):
            print(f"{rel}:{line_no}: {messages[rule_id]}: {snippet}", file=sys.stderr)
            total += 1

    if total:
        print(
            f"\ncheck_c23_patterns.py: {total} C23-pattern violation(s). "
            "Use static_assert, `= {}`, drop <stdbool.h>, and wrap bare "
            "numeric #define values in parens.",
            file=sys.stderr,
        )
        return 1
    print(f"check_c23_patterns.py: {len(targets)} file(s) scanned, 0 findings.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
