# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Shared lexing and exemption rules for the in-tree ``file:line`` citation ban.

Two tools implement that ban: :mod:`check_line_citations` is the gate that
FAILS on a stale-prone citation, and :mod:`extract_line_citations` is the
migration aid that lists the same set with the symbol each one should cite
instead. They must agree on what counts as a citation, or the aid lists work
the gate does not want and misses work it does.

They did not agree. Both carried their own byte-identical copy of
``find_comment_spans``, ``line_of_offset``, ``all_tracked_files``,
``is_in_scope`` and the five exemption regexes, plus a hand-copied version of
the same exemption cascade -- the extractor's carrying the comment "Apply the
same exemptions as the gate", which is a duplication admitting to being one.
Two copies of a rule is two places for it to drift, and drift here is silent:
both tools keep running and simply describe different trees.

The one thing deliberately NOT shared is ``EXCLUDE_PREFIXES``. The gate also
scans documentation and excludes ``docs/reference/`` wholesale; the extractor
scans sources only. That is a real difference in what each tool is for, not
an accident, so each keeps its own.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from lint_targets import language_of

#: Source extensions carrying C/C++ comments.
SOURCE_EXTS = (".c", ".h", ".cpp", ".hpp", ".cc")

#: A ``path.ext:NNN`` reference. Groups 1 and 2 are the path and the line
#: number; ``group(0)`` is the whole citation, which is what the gate reports.
CITATION_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_/.-]*\.(?:c|h|cpp|hpp|cc)):(\d+)\b")

#: An explicit waiver. The reason is mandatory -- see :func:`is_exempt`.
CITES_OK_RE = re.compile(r"CITES-OK:\s*(\S.*?)\s*$")

#: "moved from x.c:12 to ..." is history, not a live reference.
MOVED_FROM_RE = re.compile(r"moved from\s+\S+:\d+\s+to\b", re.IGNORECASE)

#: Vendor manuals under docs/reference/ have stable line-addressable content.
DOCS_REFERENCE_RE = re.compile(r"\bdocs/reference/")

#: Vendored code is not ours to re-cite.
THIRD_PARTY_RE = re.compile(r"\blibs/third_party/")

#: The annotation macro whose reason string is DO-178C 6.4.4.3 deactivation
#: evidence. Its argument is a string LITERAL, so find_comment_spans -- which
#: deliberately skips string literals -- never reaches it; find_mcdc_reason_spans
#: does, so the in-tree line-citation ban covers a deactivation reason as
#: docs/ANNOTATIONS.md says it does (#547).
MCDC_REASON_MACRO = "RA8_MCDC_DEACTIVATED"


def all_tracked_files() -> list[str]:
    """Every tracked path, for the whole-tree sweep.

    The whole-tree mode is what CI runs; the staged mode is the pre-commit
    hook's. They differ only in this enumeration, so a rule can never apply
    in one and not the other.
    """
    out = subprocess.run(
        ["git", "ls-files"],  # noqa: S607  # trusted: fixed git argv
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    return [line for line in out.splitlines() if line]


def is_in_scope(path: str, exclude_prefixes: tuple[str, ...]) -> bool:
    """Whether a source path is subject to the in-tree line-citation ban.

    First-party-ness is DERIVED, not a hardcoded root list. The old
    ``SCAN_ROOTS`` tuple (libs/, src/, tests/, examples/, port/) silently
    omitted tools/ -- the #358 defect that let a ``file.c:123``
    citation land there unseen. ``lint_targets.language_of`` decides first-party
    C by suffix, language and the shared SOUP/generated/build exclusions, so a
    new top-level directory is covered the day it lands and vendored trees stay
    out without naming each of their files.

    ``exclude_prefixes`` is a parameter rather than a module constant because
    the gate and the extractor genuinely scan different sets -- see the module
    docstring.
    """
    if not path.endswith(SOURCE_EXTS):
        return False
    if any(path.startswith(p) for p in exclude_prefixes):
        return False
    return language_of(path) == "c"


def find_comment_spans(text: str) -> list[tuple[int, int]]:  # noqa: PLR0912  # one char scanner; splitting the states hurts clarity
    """Byte spans of every C/C++ comment, block and line.

    String-literal aware, enough to skip ``"//foo"`` and ``"/* */"`` -- without
    that a URL or a format string in code would be treated as a comment and
    any citation-shaped text inside it reported.

    Returns ``(start, end)`` offsets so the caller can test whether a match
    fell inside a comment, which is the only place a citation counts.
    """
    spans: list[tuple[int, int]] = []
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        # String literal
        if ch == '"':
            i += 1
            while i < n and text[i] != '"':
                if text[i] == "\\" and i + 1 < n:
                    i += 2
                    continue
                if text[i] == "\n":
                    break
                i += 1
            i += 1
            continue
        if ch == "'":
            i += 1
            while i < n and text[i] != "'":
                if text[i] == "\\" and i + 1 < n:
                    i += 2
                    continue
                if text[i] == "\n":
                    break
                i += 1
            i += 1
            continue
        if ch == "/" and i + 1 < n:
            if text[i + 1] == "/":
                start = i
                while i < n and text[i] != "\n":
                    i += 1
                spans.append((start, i))
                continue
            if text[i + 1] == "*":
                start = i
                i += 2
                while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                    i += 1
                i = min(n, i + 2)
                spans.append((start, i))
                continue
        i += 1
    return spans


def _reason_end(text: str, start: int) -> int:
    """Offset of the ``)`` closing a reason that opened just after ``start``.

    ``start`` is the index one past the macro's opening ``(``. The walk is
    string- and paren-aware -- a ``)`` inside the reason string does not close
    the call, and a nested ``(`` is balanced -- and a single ``quote`` variable
    handles both ``"`` and ``'`` literals so the two are not duplicated.

    Args:
        text: The full source text.
        start: Offset one past the opening parenthesis.

    Returns:
        The offset of the matching close parenthesis, or ``len(text)`` when the
        call is unterminated.
    """
    n = len(text)
    depth = 1
    p = start
    quote = ""  # empty outside a literal, else the active quote character
    while p < n and depth > 0:
        ch = text[p]
        if quote:
            if ch == "\\":
                p += 2
                continue
            if ch == quote:
                quote = ""
        elif ch in "\"'":
            quote = ch
        elif ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                return p
        p += 1
    return p


def find_mcdc_reason_spans(text: str) -> list[tuple[int, int]]:
    """Byte spans of the reason argument of each ``RA8_MCDC_DEACTIVATED(...)``.

    The macro records WHY an MC/DC condition is deactivated -- DO-178C 6.4.4.3
    evidence -- and its reason is a string literal (often several adjacent
    literals across lines). :func:`find_comment_spans` deliberately skips string
    literals, so without this the in-tree line-citation ban would never reach a
    deactivation reason, though ``docs/ANNOTATIONS.md`` promises it does. This
    returns the ``(start, end)`` offsets of the text BETWEEN the macro's outer
    parentheses so the caller can scan it for a rot-prone ``file.ext:line`` token
    exactly as it scans a comment.

    A whole-token match is required, so a longer identifier merely ending in the
    macro name is skipped; the macro's own ``#define`` site yields the parameter
    name ``reason`` as its span, which carries no citation and is inert.

    Args:
        text: The full source text to scan.

    Returns:
        A list of ``(start, end)`` byte offsets, one per macro call.
    """
    spans: list[tuple[int, int]] = []
    n = len(text)
    mlen = len(MCDC_REASON_MACRO)
    i = 0
    while True:
        idx = text.find(MCDC_REASON_MACRO, i)
        if idx == -1:
            break
        i = idx + mlen
        # Whole-token match: the preceding char must not be part of an
        # identifier, or this is a longer name that only ends with the macro.
        if idx > 0 and (text[idx - 1].isalnum() or text[idx - 1] == "_"):
            continue
        # The next non-space character must be the opening parenthesis.
        k = i
        while k < n and text[k] in " \t\r\n":
            k += 1
        if k >= n or text[k] != "(":
            continue
        start = k + 1
        end = _reason_end(text, start)
        spans.append((start, end))
        i = end + 1
    return spans


def line_of_offset(text: str, offset: int) -> int:
    """1-based line number containing a byte offset.

    Counts newlines before the offset, so it stays correct on the
    comment-blanked view, whose newlines are preserved for exactly this
    reason.
    """
    return text.count("\n", 0, offset) + 1


def is_exempt(matched: str, line: str) -> bool:
    """Whether a citation on ``line`` is excused from the ban.

    THE single definition of the exemption cascade, shared by the gate and the
    migration aid so the two cannot describe different trees.

    Exempt when the citation is part of a licence identifier or an include
    directive (neither is a prose reference), names a vendor manual or vendored
    code (not ours to re-cite), records where something moved from (history,
    not a live pointer), or carries an explicit `CITES-OK: <reason>` waiver
    whose reason is non-empty -- a bare marker waives nothing, the same rule
    the gitignore-scope gate applies to its own marker.
    """
    if "SPDX-License-Identifier" in line:
        return True
    if line.lstrip().startswith("#include"):
        return True
    if DOCS_REFERENCE_RE.search(matched) or DOCS_REFERENCE_RE.search(line):
        return True
    if THIRD_PARTY_RE.search(matched) or THIRD_PARTY_RE.search(line):
        return True
    cok = CITES_OK_RE.search(line)
    if cok and cok.group(1).strip():
        return True
    return bool(MOVED_FROM_RE.search(line))
