#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""check_c23_headers.py -- enforce C23 typed enums and #pragma once headers.

Two rules that CLAUDE.md mandates but that had, until now, no automated
checker -- they were convention only, so drift was invisible until a human
happened to read the file:

1. **C23 typed enums.** Every ``enum`` / ``typedef enum`` in first-party code
   MUST specify an explicit underlying type: ``enum : <type> { ... }`` (for
   example ``typedef enum : uint8_t { ... } name_t;``). A bare ``enum {`` or a
   tagged ``enum Name {`` with no ``: <type>`` FAILS. A fixed underlying type
   pins the enum's size for ABI stability and debugger compatibility -- the
   reason CLAUDE.md's "Constants and Macros" section makes it mandatory.

2. **#pragma once headers.** First-party headers (``.h`` / ``.hpp``) MUST use
   ``#pragma once`` rather than a classic ``#ifndef`` / ``#define`` /
   ``#endif`` include guard.

Detection notes:

* Only enum *definitions* are flagged -- a definition is ``enum`` followed by
  an optional tag and then ``{``. A *reference* (``enum xz_ret ret``, a
  ``sizeof(enum foo)``, a forward ``enum foo;``) has some other token after the
  tag and is deliberately left alone. C++ scoped enums (``enum class``) do not
  occur in this C tree and are not special-cased.
* The header-guard rule is expressed as a *positive* requirement: a header
  passes iff it contains a real ``#pragma once`` directive. This is
  deliberately simpler and far less false-positive-prone than trying to
  pattern-match an ``#ifndef`` / ``#define`` / ``#endif`` *triple*: an
  ``#ifndef`` on its own is an ordinary feature-test (``#ifndef __cplusplus``)
  and must not be mistaken for an include guard. Requiring ``#pragma once``
  catches every guarded header (it lacks the pragma) without ever having to
  decide whether a given ``#ifndef`` is a guard.
* Comments and string / character literals are blanked before scanning, so an
  ``enum`` or an ``#ifndef`` written inside a comment or a string is never
  flagged, and a ``#pragma once`` mentioned inside a comment does not satisfy
  the rule.

Per-line / per-file opt-out: append ``C23HDR-OK: <reason>`` to waive a finding
(mirrors the ``MAGIC-OK`` / ``CITES-OK`` / ``AI-OK`` markers the sibling gates
use). For the enum rule the marker must sit on the offending ``enum`` line; for
the header rule it may sit anywhere in the file. The one standing use is
``port/mbedtls/inc/tf_psa_crypto_config.h``, whose ``#ifndef
PSA_CRYPTO_CONFIG_H`` guard is not an ordinary include guard at all but a
vendor-ABI sentinel: the vendored Mbed TLS SOUP tests ``#if
defined(PSA_CRYPTO_CONFIG_H)`` to confirm the crypto config file was supplied,
so the guard macro must keep being defined.

Vendored trees (``libs/third_party/``), generated tables (``libs/fonts/``) and
build output are skipped wholesale.

Usage:
    python3 scripts/checks/check_c23_headers.py                 # staged files
    python3 scripts/checks/check_c23_headers.py FILE [FILE ...] # explicit list
    python3 scripts/checks/check_c23_headers.py --all           # all tracked
    python3 scripts/checks/check_c23_headers.py --selftest      # self-check

Returns 0 on clean, 1 on findings, 2 on usage / enumeration error.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from lint_targets import is_build_output_path

REPO_ROOT = Path(__file__).resolve().parents[2]

# Path fragments that drop a file from the scan. Vendored SOUP and generated
# font tables are exempt tree-wide per CLAUDE.md; build output is handled by
# ``is_build_output_path``. Matches the sibling gates' EXCLUDE_FRAGMENTS.
EXCLUDE_FRAGMENTS = (
    "libs/third_party/",
    "libs/fonts/",
)

# The typed-enum rule applies to every first-party C/C++ translation unit and
# header; the pragma-once rule applies only to headers.
ENUM_EXTENSIONS = frozenset({".c", ".h", ".cpp", ".hpp"})
HEADER_EXTENSIONS = frozenset({".h", ".hpp"})

# Per-line / per-file opt-out marker (written reason required by convention).
OPT_OUT = "C23HDR-OK"

# An enum introduced by the keyword ``enum``, then an OPTIONAL tag identifier,
# then the first following non-space token. That token disambiguates:
#   ``{`` -> this is a DEFINITION body, and (having no ``:`` before it) it is
#            UNTYPED -> a violation.
#   ``:`` -> a fixed underlying type follows -> typed, compliant.
#   else -> a reference / use / forward declaration -> not our concern.
# DOTALL lets the leading ``\s*`` swallow newlines so a ``typedef enum\n{``
# split across lines is still recognised as one definition.
_ENUM_RE = re.compile(
    r"\benum\b\s*(?:[A-Za-z_]\w*\s*)?(?P<next>.)",
    re.DOTALL,
)

# A real ``#pragma once`` directive (in code, after comment/string blanking).
_PRAGMA_ONCE_RE = re.compile(r"^\s*#\s*pragma\s+once\b", re.MULTILINE)


def _strip_comments_and_strings(text: str) -> str:
    """Blank comment and literal bytes to spaces, preserving every position.

    Newlines survive so line numbers computed against the blanked view remain
    valid for the original source. A naive char-by-char state machine is used
    rather than a regex because nested-looking quote / comment interactions
    make a single regex fragile.

    Args:
        text: The original source text of one file.

    Returns:
        The same text with the interior of every ``//`` / ``/* */`` comment and
        every string / char literal replaced by spaces (newlines kept).
    """
    out: list[str] = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if c == "/" and nxt == "*":
            out.append("  ")
            i += 2
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            if i < n:
                out.append("  ")
                i += 2
            continue
        if c in {'"', "'"}:
            quote = c
            out.append(" ")
            i += 1
            while i < n and text[i] != quote:
                if text[i] == "\\" and i + 1 < n:
                    out.append("  ")
                    i += 2
                    continue
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            if i < n:
                out.append(" ")
                i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def _line_of(text: str, offset: int) -> int:
    """Return the 1-based line number of a character offset in ``text``.

    Args:
        text: The text the offset indexes into.
        offset: A 0-based character offset.

    Returns:
        The 1-based line number containing ``offset``.
    """
    return text.count("\n", 0, offset) + 1


def enum_violations(text: str) -> list[tuple[int, str]]:
    """Find every untyped ``enum`` definition in one file's text.

    Only enum definitions (``enum [tag] {``) are considered; references and
    forward declarations are skipped. A definition whose ``{`` is preceded by a
    ``: <type>`` clause is compliant and is not reported.

    Args:
        text: The original source text of one file.

    Returns:
        A list of ``(line_no, snippet)`` for each untyped enum definition, with
        opt-out (``C23HDR-OK``) lines already removed.
    """
    stripped = _strip_comments_and_strings(text)
    orig_lines = text.splitlines()
    violations: list[tuple[int, str]] = []
    for match in _ENUM_RE.finditer(stripped):
        if match.group("next") != "{":
            continue  # ``:`` is typed; any other token is a reference/use
        line_no = _line_of(stripped, match.start())
        raw = orig_lines[line_no - 1] if line_no - 1 < len(orig_lines) else ""
        if OPT_OUT in raw:
            continue
        violations.append((line_no, raw.strip()))
    return violations


def header_lacks_pragma_once(text: str) -> bool:
    """Return whether a header's text is missing a real ``#pragma once``.

    The check runs against the comment/string-blanked view so a ``#pragma
    once`` written inside a comment does not count. A file carrying the
    ``C23HDR-OK`` opt-out anywhere is treated as compliant.

    Args:
        text: The original source text of a header file.

    Returns:
        True when the header has no ``#pragma once`` directive and is not
        waived; False otherwise.
    """
    if OPT_OUT in text:
        return False
    stripped = _strip_comments_and_strings(text)
    return _PRAGMA_ONCE_RE.search(stripped) is None


def find_violations(path: Path) -> list[tuple[int, str]]:
    """Report every C23 typed-enum / pragma-once violation in one file.

    The enum rule is applied to any in-scope C/C++ file; the pragma-once rule
    is applied only to headers.

    Args:
        path: The file to scan.

    Returns:
        A list of ``(line_no, message)`` findings; an unreadable file yields an
        empty list rather than raising. A header missing ``#pragma once`` is
        reported at line 1.
    """
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    findings: list[tuple[int, str]] = []
    if path.suffix.lower() in ENUM_EXTENSIONS:
        for line_no, snippet in enum_violations(text):
            findings.append(
                (line_no, f"untyped enum -- add `: <type>` (e.g. `enum : uint8_t`): {snippet}")
            )
    if path.suffix.lower() in HEADER_EXTENSIONS and header_lacks_pragma_once(text):
        findings.append((1, "header has no `#pragma once` (classic include guards are banned)"))
    findings.sort()
    return findings


def _is_excluded(rel: str) -> bool:
    """Return whether a repo-relative path is outside the scan scope.

    Args:
        rel: A repo-relative path string.

    Returns:
        True for vendored / generated / build-output paths, False otherwise.
    """
    return is_build_output_path(rel) or any(frag in rel for frag in EXCLUDE_FRAGMENTS)


def _in_scope(path: Path) -> bool:
    """Return whether a path is a first-party file this gate scans.

    Args:
        path: The candidate file path (absolute or repo-relative).

    Returns:
        True when the suffix is one the gate handles and the path is not
        excluded.
    """
    if path.suffix.lower() not in ENUM_EXTENSIONS:
        return False
    rel = str(path.relative_to(REPO_ROOT)) if path.is_relative_to(REPO_ROOT) else str(path)
    return not _is_excluded(rel)


def _git_lines(args: list[str]) -> list[str]:
    """Run a git command under the repo root and return its stdout lines.

    Args:
        args: The git sub-command and its arguments (without the ``git`` prefix).

    Returns:
        The non-empty stdout lines. Exits with status 2 if git fails.
    """
    proc = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool
        ["git", *args],  # noqa: S607 -- git is a fixed dev-tool name
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr)
        sys.stderr.write(f"check_c23_headers.py: FATAL -- `git {args[0]}` failed\n")
        sys.exit(2)
    return [ln for ln in proc.stdout.splitlines() if ln]


def staged_files() -> list[Path]:
    """Return the in-scope files staged for the in-progress commit.

    Returns:
        Absolute paths of added/copied/modified/renamed staged files that are
        in scope and still present on disk.
    """
    names = _git_lines(["diff", "--cached", "--name-only", "--diff-filter=ACMR"])
    out: list[Path] = []
    for name in names:
        path = REPO_ROOT / name
        if path.is_file() and _in_scope(path):
            out.append(path)
    return out


def all_files() -> list[Path]:
    """Return every tracked first-party file in scope, for the ``--all`` sweep.

    Returns:
        Absolute paths of tracked ``.c`` / ``.h`` / ``.cpp`` / ``.hpp`` files
        that are not vendored, generated, or build output.
    """
    names = _git_lines(["ls-files", "--", "*.c", "*.h", "*.cpp", "*.hpp"])
    return [REPO_ROOT / name for name in names if _in_scope(REPO_ROOT / name)]


def _enum_cases() -> list[tuple[bool, str]]:
    """Return the enum-rule self-check cases as ``(failed, message)`` pairs.

    Each case maps a fragment through :func:`enum_violations` and records
    whether the observed behaviour differs from the specified behaviour: the
    untyped/tagged-untyped definitions must fire; typed forms, references and
    commented-out enums must stay silent; the opt-out must be honoured.

    Returns:
        One ``(failed, message)`` tuple per assertion; ``failed`` is True when
        the detector behaved wrongly.
    """
    fires = lambda frag: bool(enum_violations(frag))  # noqa: E731 -- local alias keeps the table terse
    return [
        (not fires("typedef enum { k_a = 0 } foo_t;"), "untyped `typedef enum {` not flagged"),
        (not fires("enum bearer { k_a = 1 };"), "untyped tagged `enum bearer {` not flagged"),
        (fires("typedef enum : uint8_t { k_a = 0 } foo_t;"), "typed `enum : uint8_t {` flagged"),
        (fires("enum bearer : uint16_t { k_a = 1 };"), "typed tagged `enum ... :` flagged"),
        (fires("static int map(enum xz_ret ret) { return ret; }"), "enum parameter ref flagged"),
        (fires("size_t n = sizeof(enum foo);"), "enum sizeof reference flagged"),
        (fires("/* typedef enum { k_a } t; */"), "untyped enum in a comment flagged"),
        (not fires("enum { k_x = 1 };  /* real */"), "untyped enum w/ trailing comment missed"),
        (fires("enum { k_x = 1 };  /* C23HDR-OK: reason */"), "enum opt-out not honoured"),
    ]


def _header_cases() -> list[tuple[bool, str]]:
    """Return the header-rule self-check cases as ``(failed, message)`` pairs.

    An ``#ifndef`` include-guarded header must fire; a ``#pragma once`` header
    must not; a ``#pragma once`` buried in a comment must not satisfy the rule;
    the opt-out must be honoured.

    Returns:
        One ``(failed, message)`` tuple per assertion; ``failed`` is True when
        the detector behaved wrongly.
    """
    guarded = "#ifndef FOO_H\n#define FOO_H\nint foo(void);\n#endif\n"
    lacks = header_lacks_pragma_once
    return [
        (not lacks(guarded), "`#ifndef` include-guarded header not flagged"),
        (lacks("#pragma once\nint foo(void);\n"), "`#pragma once` header wrongly flagged"),
        (not lacks("/* #pragma once */\nint foo(void);\n"), "commented `#pragma once` accepted"),
        (lacks(guarded + "/* C23HDR-OK: reason */\n"), "header opt-out not honoured"),
    ]


def _selftest() -> int:
    """Assert both rules fire on the real defect and stay quiet on legal forms.

    Returns:
        0 when every direction of both rules behaves as specified, 1 otherwise.
    """
    print("check_c23_headers.py --selftest")
    failures = [msg for failed, msg in _enum_cases() + _header_cases() if failed]
    if failures:
        for msg in failures:
            print(f"  FAIL: {msg}", file=sys.stderr)
        print(f"check_c23_headers.py --selftest: {len(failures)} failure(s)", file=sys.stderr)
        return 1
    print("check_c23_headers.py --selftest: OK")
    return 0


def _report(candidates: list[Path]) -> int:
    """Scan ``candidates`` and print any findings.

    Args:
        candidates: The in-scope files to scan.

    Returns:
        1 if any violation was found, 0 otherwise.
    """
    total = 0
    for path in candidates:
        rel = path.relative_to(REPO_ROOT) if path.is_relative_to(REPO_ROOT) else path
        for line, message in find_violations(path):
            print(f"{rel}:{line}: {message}", file=sys.stderr)
            total += 1
    if total:
        print(
            f"\n{total} C23 header/enum violation(s). Every enum needs an explicit "
            "underlying type (`enum : uint8_t { ... }`) and every header must use "
            f"`#pragma once`. A genuine exception carries a `{OPT_OUT}: <reason>` "
            "marker.",
            file=sys.stderr,
        )
        return 1
    print(f"check_c23_headers.py: {len(candidates)} file(s) scanned, 0 findings.")
    return 0


def main(argv: list[str]) -> int:
    """Enforce C23 typed-enum underlying types and pragma-once headers.

    ``--selftest`` proves the detector in both directions; ``--all`` sweeps the
    tree; an explicit file list scans just those files; otherwise the staged
    set is scanned (how the pre-commit hook stays fast).

    Args:
        argv: The full process argument vector (``sys.argv``).

    Returns:
        0 clean, 1 on findings, 2 on usage / enumeration error.
    """
    parser = argparse.ArgumentParser(description="C23 typed-enum + pragma-once checker")
    parser.add_argument("--all", action="store_true", help="scan all tracked files")
    parser.add_argument("--selftest", action="store_true", help="assert both rules, both ways")
    parser.add_argument("files", nargs="*", type=Path, help="explicit file list (e.g. staged)")
    args = parser.parse_args(argv[1:])

    if args.selftest:
        return _selftest()
    if args.all:
        candidates = all_files()
    elif args.files:
        candidates = [p if p.is_absolute() else REPO_ROOT / p for p in args.files]
        candidates = [p for p in candidates if p.is_file() and _in_scope(p)]
    else:
        candidates = staged_files()

    return _report(candidates)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
