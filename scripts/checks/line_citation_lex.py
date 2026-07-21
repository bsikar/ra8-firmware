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

#: Roots subject to the ban.
SCAN_ROOTS = ("libs/", "src/", "tests/", "examples/", "port/")

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

    Three conditions, all required: a source extension, a location under one
    of the scan roots, and no excluded prefix. The root test is what keeps
    vendored trees out without needing to name each of their files.

    ``exclude_prefixes`` is a parameter rather than a module constant because
    the gate and the extractor genuinely scan different sets -- see the module
    docstring.
    """
    if not path.endswith(SOURCE_EXTS):
        return False
    if not any(path.startswith(r) for r in SCAN_ROOTS):
        return False
    return not any(path.startswith(p) for p in exclude_prefixes)


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
