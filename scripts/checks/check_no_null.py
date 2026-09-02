#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""check_no_null.py -- enforce the C23 nullptr-only rule on first-party code.

Project policy: first-party C must use ``nullptr`` instead of ``NULL`` for
null pointer constants. This script walks staged or all-tracked source files
and rejects bare ``NULL`` tokens in code positions. It allows NULL in:

  * comment lines and inline `/* ... */` / `// ...` comments
  * string literals
  * Doxygen annotation prose
  * UX_NULL / similar vendor macros (USBX expects literal NULL)
  * exact generated-source paths registered by the lint-coverage manifest

Scope is DERIVED from git ls-files (#358), so tools/ -- host tooling held to
the same C23 bar, and silently omitted by the old ROOT_DIRS tuple -- is now in
scope, along with every future top-level directory. Vendored SOUP
(libs/third_party/, libs/ra8_fonts/, port/threadx/, ...) is skipped wholesale, and
one first-party tree is exempt for a stated reason (see EXEMPT_PREFIXES):
tests/ (NULL is deliberate null-guard stimulus).

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
import tempfile
from collections.abc import Iterable

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from lint_coverage_rules import PATH_CLASS
from lint_targets import first_party_paths, is_build_output_path
from selftest_assert import expect, report

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]

EXTENSIONS = (".c", ".h", ".cpp", ".hpp")

# Vendored SOUP the nullptr rule never governs. first_party_paths already drops
# these for the --all sweep; needs_check re-checks them so an explicitly named
# vendored file (a pre-commit staging edge case) is skipped as well.
SOUP_PREFIXES = (
    "libs/third_party/",
    "apps/shared_libs/third_party/",
    "libs/ra8_fonts/",
    "tools/vela/generated/",
    "port/threadx/",
)

# Scope recorded here, NOT as a directory allowlist (#358). Enumeration is
# derived from git ls-files, so tools/ -- host tooling held to the same C23 bar
# per CLAUDE.md, and silently omitted by the old ROOT_DIRS tuple -- and every
# future top-level directory are in scope automatically. One first-party tree
# is deliberately OUT, for a stated reason:
EXEMPT_PREFIXES = (
    # Unit tests pass NULL as deliberate stimulus to exercise null-pointer
    # guards -- `TEST_ASSERT_EQ(k_ra8_err_null_ptr, fn(NULL, ...))`. Requiring
    # nullptr there fights the test rather than the code, the same reason
    # check_magic_numbers.py holds tests/ exempt.
    "tests/",
)

# Generator-owned output is governed by reproducible regeneration/diff checks,
# not handwritten C23 spelling rules.  This is an exact manifest lookup: a
# future file merely resembling generated output remains in scope.
GENERATED_SOURCE_PATHS = frozenset(
    path for path, classification in PATH_CLASS.items() if classification == "generated-source"
)

# bare NULL token in a code context. \bNULL\b matches the identifier;
# we strip comments and strings first so this only fires in code.
NULL_RE = re.compile(r"\bNULL\b")


# Strip C/C++ inline comments and string/char literals so we don't
# match NULL inside them. Naive but adequate for this use case.
def _strip_noncode(line: str) -> str:  # noqa: PLR0912, PLR0915  # char-by-char state machine, splitting hurts readability
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
    """Report every use of ``NULL`` in one file.

    C23 spells the null pointer constant ``nullptr``, which is typed; ``NULL``
    is a macro that expands to an untyped 0 and so silently satisfies an
    integer parameter. That is the defect this catches, not the spelling.

    Returns ``(line_no, line_text)`` per finding; an unreadable file yields an
    empty list rather than raising.
    """
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


def _in_scope(rel: str) -> bool:
    """Whether repo-relative ``rel`` is first-party C the nullptr rule governs.

    Pure and total, so the selftest asserts scope on synthetic paths without
    touching the tree: a re-narrowing that drops tools/ fails the selftest
    instead of passing green.
    """
    if not rel.endswith(EXTENSIONS):
        return False
    if (
        rel.startswith(EXEMPT_PREFIXES)
        or "/tests/" in rel
        or rel.startswith(SOUP_PREFIXES)
        or rel in GENERATED_SOURCE_PATHS
    ):
        return False
    return not is_build_output_path(rel)


def needs_check(path: pathlib.Path) -> bool:
    """Whether a CLI-supplied path is first-party C subject to the nullptr rule."""
    if path.suffix.lower() not in EXTENSIONS:
        return False
    try:
        rel = path.resolve().relative_to(REPO_ROOT).as_posix()
    except ValueError:
        # Outside the repo (an isolated selftest fixture): the suffix already
        # matched and no repo-relative prefix rule can apply, so it is in scope.
        return True
    return _in_scope(rel)


def iter_all_files() -> Iterable[pathlib.Path]:
    """Every first-party C file the rule governs, for the ``--all`` sweep.

    Derived from git ls-files via first_party_paths (which already removes
    vendored SOUP, generated tables, build output and the vendored port/threadx
    tree), minus the two documented EXEMPT_PREFIXES. A newly-added top-level
    directory of first-party C is covered the day it lands -- no allowlist.
    """
    for rel in first_party_paths(EXTENSIONS):
        if _in_scope(rel):
            yield REPO_ROOT / rel


# ---------------------------------------------------------------------------
# Selftest -- both directions, plus a scope assertion under tools/, the root
# ROOT_DIRS silently omitted until #358. A scope that quietly re-narrows must
# fail here, not report a clean tree over files it stopped scanning.
# ---------------------------------------------------------------------------
_BAD_FIXTURE = "int f(void) { char *p = NULL; return p == NULL; }\n"
_GOOD_FIXTURE = (
    "int f(void) { char *p = nullptr;   // NULL in a comment is fine\n"
    '  const char *s = "NULL literal";  // and in a string literal\n'
    "  return (p == nullptr) && (UX_NULL == p); }\n"
)


def selftest() -> int:
    """Prove bare NULL fires, legal constructs stay quiet, and the scope holds."""
    print("check_no_null.py --selftest")
    failures: list[str] = []
    with tempfile.TemporaryDirectory() as tmp:
        bad = pathlib.Path(tmp) / "bad.c"
        bad.write_text(_BAD_FIXTURE, encoding="utf-8")
        good = pathlib.Path(tmp) / "good.c"
        good.write_text(_GOOD_FIXTURE, encoding="utf-8")
        expect(bool(find_violations(bad)), "bare NULL in code fires", failures)
        expect(
            not find_violations(good),
            "nullptr / vendor macro / comment / string stays quiet",
            failures,
        )
    expect(
        _in_scope("tools/mkbookimg/src/mkbookimg.c"),
        "tools/ is in scope (ROOT_DIRS omitted it before #358)",
        failures,
    )
    expect(not _in_scope("tests/test_x.c"), "tests/ exempt (deliberate NULL stimulus)", failures)
    generated = "libs/ra8_c6link/src/ra8_media_download.pb-c.c"
    expect(
        generated in GENERATED_SOURCE_PATHS and not _in_scope(generated),
        "registered generated source is exempt",
        failures,
    )
    expect(
        _in_scope("libs/ra8_c6link/src/future_generated.pb-c.c"),
        "generated-looking future source is not automatically exempt",
        failures,
    )
    expect(not _in_scope("libs/third_party/threadx/src/tx.c"), "platform SOUP exempt", failures)
    expect(
        not _in_scope("apps/shared_libs/third_party/miniz/miniz.c"),
        "app SOUP exempt",
        failures,
    )
    expect(
        _in_scope("apps/shared_libs/compress/src/compress.c"),
        "adjacent app first-party code remains in scope",
        failures,
    )
    return report(failures)


def main() -> int:
    """Fail on any use of ``NULL`` where C23 ``nullptr`` is required.

    ``--all`` sweeps the tree; otherwise only the named files are checked,
    which is how the pre-commit hook stays fast.

    Returns 1 listing each use, 0 when clean.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--all", action="store_true", help="scan all tracked source files")
    parser.add_argument(
        "--selftest", action="store_true", help="prove the rule fires and the scope holds"
    )
    parser.add_argument(
        "files", nargs="*", type=pathlib.Path, help="explicit file list (e.g. staged files)"
    )
    args = parser.parse_args()

    if args.selftest:
        return selftest()

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
