#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""check_no_goto_setjmp.py -- NASA Power-of-10 Rule 1 textual backstop.

NASA/JPL Power-of-10 Rule 1 forbids unstructured control flow: no ``goto``,
no ``setjmp`` / ``longjmp``, and no recursion. Three of those four constructs
-- ``goto``, ``setjmp``, ``longjmp`` -- are single keywords / library calls
that a textual scan can find reliably, so this gate closes them off with a
parser-independent sweep of first-party C/C++.

Why a dedicated gate. Until now ``goto`` / ``setjmp`` were enforced only
indirectly, via the MISRA cppcheck ratchet (Rule 15.1 forbids ``goto``,
Rule 21.4 forbids ``<setjmp.h>``). That ratchet runs cppcheck at
``--std=c11`` because the bundled cppcheck (2.20) cannot parse C23 -- the
codebase's ``enum : uint8_t`` typed enums and ``[[...]]`` attributes raise
``syntaxError`` and the affected translation units are only partially parsed
(see ``scripts/checks/misra_check_inner.sh`` and ADR-0002). A construct on a
line cppcheck skipped is a construct the ratchet never rules on. A textual
scan does not depend on a parse, so it covers the whole tree uniformly and
closes that blind spot for these three tokens.

Recursion is deliberately OUT of scope here: detecting it needs a call graph,
which is covered separately by ``scripts/checks/annot_rules.py``
(``RA8_NO_RECURSION``) and MISRA Rule 17.2.

The three tokens are matched only in CODE positions. Occurrences inside
comments and string / character literals are prose, not control flow, and are
never flagged -- the tree has a ``goto`` inside a comment in
``libs/ra8_hal/src/ra8_flash_config.c`` and ``setjmp`` / ``longjmp`` named in
the prose of ``libs/ra8_core/inc/ra8_err.h`` and
``libs/ra8_core/src/ra8_exception.c``, none of which is a violation.

Scope is the firmware and host-tool tree -- ``libs/``, ``src/``,
``examples/``, ``port/``, ``tools/``. ``tests/`` is deliberately out of
scope: the host unit-test harness legitimately uses ``setjmp`` / ``longjmp``
to trap the firmware's ``[[noreturn]]`` fatal paths under test, which is test
scaffolding rather than firmware control flow (the same reason ``tests/`` is
exempt from MC/DC re-test and the magic-number gate). Vendored trees under
``libs/third_party/``, generated data under ``libs/fonts/``, and build output
are skipped wholesale.

Usage::

    check_no_goto_setjmp.py                    # scan staged files (pre-commit)
    check_no_goto_setjmp.py FILE [FILE ...]    # scan an explicit file list
    check_no_goto_setjmp.py --all              # scan every tracked source file
    check_no_goto_setjmp.py --selftest         # self-check the detector

Returns 0 on clean, 1 on findings, 2 on usage error.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys
from collections.abc import Iterable

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from lint_targets import is_build_output_path

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]

# First-party roots this gate is responsible for: the firmware libraries, the
# shared internals, the applications, the ThreadX port glue, and the host
# tools. Vendored / generated trees are subtracted below; a build directory
# under any of them is subtracted by is_build_output_path (the shared #377
# definition).
#
# `tests/` is deliberately NOT a root. NASA Power-of-10 Rule 1 governs the
# firmware; the host unit-test harness under tests/ legitimately uses
# setjmp/longjmp to trap the firmware's [[noreturn]] fatal paths
# (internal_ra8_fatal_error and friends) so a test can observe an abort without
# aborting the test process. That is test scaffolding, not firmware control
# flow -- the same reason tests/ is exempt from MC/DC re-test and the
# magic-number gate, and why sibling check_no_null.py scopes to
# libs/src/port/examples only.
ROOT_DIRS = ("libs", "src", "examples", "port", "tools")

# Path fragments that exclude a file from the scan. Vendored SOUP and generated
# font tables are exempt -- their control flow is the upstream maintainer's
# call, and CLAUDE.md already names both trees. Matches the sibling gates'
# EXCLUDE_FRAGMENTS.
EXCLUDE_FRAGMENTS = (
    "libs/third_party/",
    "libs/fonts/",
)

EXTENSIONS = {".c", ".h", ".cpp", ".hpp"}

# The banned control-flow tokens. \b anchors each to a full identifier so a
# name that merely CONTAINS one is left alone: `goto_label` (trailing word
# char), `sigsetjmp` / `_setjmp` (leading word char), and `longjmp_buf` are not
# matches. The three tokens are matched only after comments and string / char
# literals are blanked, so this fires in code positions exclusively.
BANNED_RE = re.compile(r"\b(goto|setjmp|longjmp)\b")


# Strip C/C++ inline comments and string / char literals from a single line so
# a banned token inside one is not matched. Naive but adequate; the multi-line
# block-comment state is threaded by the caller. Mirrors check_no_null.py's
# helper of the same purpose.
def _strip_noncode(line: str) -> str:  # noqa: PLR0912  # char-by-char state machine, splitting hurts readability
    out: list[str] = []
    i = 0
    in_str = False
    in_chr = False
    in_bc = False
    n = len(line)
    while i < n:
        c = line[i]
        nxt = line[i + 1] if i + 1 < n else ""
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


def scan_text(text: str) -> list[tuple[int, str, str]]:
    """Report every code-position ``goto`` / ``setjmp`` / ``longjmp`` in ``text``.

    Comments (both ``//`` and multi-line ``/* ... */``) and string / character
    literals are blanked before the token match, so a banned word appearing in
    prose is not reported -- that is the whole point of the gate, since the tree
    legitimately names these tokens in comments.

    Args:
        text: The full source text of one translation unit.

    Returns:
        One ``(line_no, token, line_text)`` tuple per finding, in file order.
    """
    findings: list[tuple[int, str, str]] = []
    in_block_comment = False
    for n, raw in enumerate(text.splitlines(), 1):
        cur = raw
        # Continue an open /* ... */ from a previous line.
        if in_block_comment:
            end = cur.find("*/")
            if end == -1:
                continue
            cur = cur[end + 2 :]
            in_block_comment = False
        # A /* on this line that does not close opens a block comment; keep the
        # code before it and swallow the rest.
        bo = cur.find("/*")
        if bo != -1 and cur.find("*/", bo + 2) == -1:
            cur = cur[:bo]
            in_block_comment = True
        code = _strip_noncode(cur)
        findings.extend((n, m.group(1), raw.strip()) for m in BANNED_RE.finditer(code))
    return findings


def find_violations(path: pathlib.Path) -> list[tuple[int, str, str]]:
    """Report every banned control-flow token in one file.

    An unreadable file yields an empty list rather than raising, matching the
    sibling gates: a file that cannot be read is not evidence of a violation.

    Args:
        path: The source file to scan.

    Returns:
        One ``(line_no, token, line_text)`` tuple per finding.
    """
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    return scan_text(text)


def needs_check(path: pathlib.Path) -> bool:
    """Whether ``path`` is a first-party C/C++ file subject to this gate.

    Args:
        path: Candidate path, absolute or repo-relative.

    Returns:
        True when the suffix is C/C++, the path is under a first-party root,
        and it is neither vendored / generated nor build output.
    """
    if path.suffix.lower() not in EXTENSIONS:
        return False
    text = str(path).replace("\\", "/")
    if is_build_output_path(text):
        return False
    if any(frag in text for frag in EXCLUDE_FRAGMENTS):
        return False
    rel = path.relative_to(REPO_ROOT) if path.is_relative_to(REPO_ROOT) else path
    return rel.parts[0] in ROOT_DIRS if rel.parts else False


def _git_lines(args: list[str]) -> list[str]:
    """Run a ``git`` command under the repo root and return its stdout lines.

    Args:
        args: Argument vector following ``git`` (e.g. ``["ls-files"]``).

    Returns:
        Non-empty stripped stdout lines; empty on any git failure.
    """
    proc = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool
        ["git", *args],  # noqa: S607 -- trusted: fixed git argv
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr)
        return []
    return [line.strip() for line in proc.stdout.splitlines() if line.strip()]


def iter_tracked_files() -> Iterable[pathlib.Path]:
    """Yield every checkable tracked file, for the ``--all`` sweep.

    Enumerating via ``git ls-files`` keeps gitignored build artifacts out and
    picks up a new first-party root the day it is added -- there is no allowlist
    to forget.

    Returns:
        An iterable of absolute paths that pass ``needs_check``.
    """
    for rel in _git_lines(["ls-files", "--cached", "--others", "--exclude-standard"]):
        path = REPO_ROOT / rel
        if needs_check(path):
            yield path


def iter_staged_files() -> Iterable[pathlib.Path]:
    """Yield every checkable staged file, for the default pre-commit sweep.

    Returns:
        An iterable of absolute paths of added/copied/modified/renamed staged
        files that pass ``needs_check``.
    """
    for rel in _git_lines(["diff", "--cached", "--name-only", "--diff-filter=ACMR"]):
        path = REPO_ROOT / rel
        if needs_check(path):
            yield path


# ---------------------------------------------------------------------------
# Selftest
#
# Both directions are asserted against in-memory fixtures fed through the same
# scan_text() the gate uses: a real goto / setjmp / longjmp in code MUST fire,
# and a token that appears only inside a comment or string literal -- plus
# correct code that avoids all three -- MUST stay silent. A detector that
# quietly stopped matching would report a clean tree, which is worse than no
# gate at all.
# ---------------------------------------------------------------------------

_BAD_FIXTURE = """\
void f(int *buf)
{
    goto done;
done:
    setjmp(buf);
    longjmp(buf, 1);
}
"""

_GOOD_FIXTURE = """\
/* This routine once used a goto done; jump -- rewritten as a loop. */
// setjmp / longjmp are banned by NASA Power-of-10 Rule 1.
static const char *s_note = "no goto, setjmp, or longjmp here";

void f(int *buf)
{
    (void)buf;
    for (int goto_count = 0; goto_count < 4; ++goto_count) {
        continue;
    }
}
"""


def selftest() -> int:
    """Prove the detector fires on real violations and is silent on prose.

    Returns:
        0 when both directions hold, 1 when either fails.
    """
    failures: list[str] = []

    fired = {tok for _, tok, _ in scan_text(_BAD_FIXTURE)}
    failures.extend(
        f"  must-fire: bad fixture did not report `{token}`"
        for token in ("goto", "setjmp", "longjmp")
        if token not in fired
    )

    quiet = scan_text(_GOOD_FIXTURE)
    failures.extend(
        f"  must-stay-quiet: good fixture reported `{tok}` at line {ln}" for ln, tok, _ in quiet
    )

    if failures:
        sys.stderr.write("check_no_goto_setjmp.py --selftest: FAILED\n")
        sys.stderr.write("\n".join(failures) + "\n")
        return 1

    print("check_no_goto_setjmp.py --selftest: OK (fires on code, silent on comments/strings).")
    return 0


def main() -> int:
    """Fail on any code-position ``goto`` / ``setjmp`` / ``longjmp``.

    With no arguments the staged files are scanned, which is how the pre-commit
    hook stays fast; ``--all`` sweeps every tracked source file; an explicit
    file list scans exactly those. ``--selftest`` self-checks the detector.

    Returns:
        1 listing each finding, 0 when clean, 2 on a usage error.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--all", action="store_true", help="scan all tracked source files")
    parser.add_argument("--selftest", action="store_true", help="self-check the detector and exit")
    parser.add_argument(
        "files", nargs="*", type=pathlib.Path, help="explicit file list (e.g. staged files)"
    )
    args = parser.parse_args()

    if args.selftest:
        return selftest()

    if args.all:
        candidates = list(iter_tracked_files())
    elif args.files:
        candidates = [p for p in args.files if needs_check(p)]
    else:
        candidates = list(iter_staged_files())

    total = 0
    for path in candidates:
        rel = path.relative_to(REPO_ROOT) if path.is_relative_to(REPO_ROOT) else path
        for line, token, snippet in find_violations(path):
            print(
                f"{rel}:{line}: `{token}` is banned "
                f"(NASA Power-of-10 Rule 1: no goto/setjmp/longjmp): {snippet}",
                file=sys.stderr,
            )
            total += 1

    if total:
        print(
            f"\n{total} banned control-flow token(s) found. NASA Power-of-10 "
            "Rule 1 forbids goto, setjmp, and longjmp. Restructure the control "
            "flow; the tokens are allowed only inside comments and string "
            "literals.",
            file=sys.stderr,
        )
        return 1
    print(f"check_no_goto_setjmp.py: {len(candidates)} file(s) scanned, 0 findings.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
