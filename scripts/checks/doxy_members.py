# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""The member gate: every enum value, struct/union member and macro is documented.

CLAUDE.md ("Doxygen Documentation Requirements") demands documentation on each
one -- an inline ``/**< ... */`` or a preceding ``/** ... */`` block on every
aggregate member, and a ``@brief``/``@def`` block on every macro.

Separate from the function gate because it asks a different question of a
different view of the source (see :mod:`doxy_lex`) over a wider scope (see
:mod:`doxy_scope`), and because the two were enforced at different times: the
function gate has always been strict, the member gate became strict in #246.
"""

from __future__ import annotations

import re
from pathlib import Path

from doxy_lex import _body_depth, _first_code_offset, _match_brace, blank_noncode
from doxy_scope import repo_root

_AGG_KW_RE = re.compile(r"\b(enum|struct|union)\b")
_AGG_TERMINATORS = frozenset(";=,}")


def find_aggregate_bodies(code: str) -> list[tuple[str, int, int]]:  # noqa: PLR0912  # char scanner; a helper per branch hurts clarity
    """Locate enum/struct/union *definitions* (those with a ``{ ... }`` body).

    ``code`` must be the blanked code-only view so keywords/braces inside
    comments and strings are already gone. Returns a list of
    ``(kind, open_idx, close_idx)`` for every definition, including nested ones
    (each nested aggregate is reported on its own; the parent scan skips the
    nested body via brace depth).
    """
    results = []
    n = len(code)
    for m in _AGG_KW_RE.finditer(code):
        kind = m.group(1)
        p = m.end()
        depth = 0
        open_idx = None
        # Between the keyword and a definition's `{` C allows only an
        # optional tag and attributes. A `*` or a completed parameter list
        # means the keyword is an elaborated type specifier on a
        # declarator instead -- `struct os_mbuf* f(void) { ... }` is a
        # function returning a pointer, not a struct definition, and
        # reading its body as a member list demands doc comments on the
        # statements inside it.
        declarator = False
        while p < n:
            ch = code[p]
            if ch in "([":
                depth += 1
            elif ch in ")]":
                if depth == 0:
                    break
                depth -= 1
                if depth == 0:
                    declarator = True
            elif depth == 0:
                if ch == "*":
                    declarator = True
                elif ch == "{":
                    if not declarator:
                        open_idx = p
                    break
                elif ch in _AGG_TERMINATORS:
                    break
            p += 1
        if open_idx is None:
            continue
        close_idx = _match_brace(code, open_idx)
        if close_idx is None:
            continue
        results.append((kind, open_idx, close_idx))
    return results


def member_name(kind: str, seg_code: str) -> str:
    """Best-effort declarator name for a member segment (for the report only)."""
    s = seg_code.strip()
    if not s:
        return "(anon)"
    if kind == "enum":
        m = re.match(r"([A-Za-z_]\w*)", s)
        return m.group(1) if m else "(anon)"
    if "}" in s:  # named field after a nested aggregate: "union { ... } name"
        s = s[s.rindex("}") + 1 :]
    s = re.sub(r":\s*\d+", "", s)  # drop bitfield width
    s = re.sub(r"\[[^\]]*\]", "", s)  # drop array subscripts
    fp = re.search(r"\(\s*\*\s*([A-Za-z_]\w*)", s)  # function-pointer member
    if fp:
        return fp.group(1)
    ids = re.findall(r"[A-Za-z_]\w*", s)
    return ids[-1] if ids else "(anon)"


def audit_aggregate(  # noqa: PLR0913  # body slice
    kind: str,
    code: str,
    comments: list[tuple[int, int, str | None]],
    open_idx: int,
    close_idx: int,
    rel: str,
) -> list[tuple[str, int, str, str, str]]:
    """Yield offender rows for undocumented members of one aggregate body.

    A member is documented when it carries either a preceding doc block
    (``/** ... */`` etc.) in the gap before its code, or an inline doc comment
    (``/**< ... */`` etc.) trailing it at the same brace depth. Both forms
    satisfy CLAUDE.md. Inline docs nested one level deeper belong to the inner
    aggregate (audited on its own pass), not to the enclosing named field.
    """
    delim = "," if kind == "enum" else ";"
    body_start, body_end = open_idx, close_idx

    delims = []
    depth = 0
    i = body_start + 1
    while i < body_end:
        ch = code[i]
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        elif depth == 0 and ch == delim:
            delims.append(i)
        i += 1

    lows = [body_start, *delims]
    highs = [*delims, body_end]
    code_starts = [_first_code_offset(code, lo, hi) for lo, hi in zip(lows, highs)]

    rows = []
    for idx, (lo, cs) in enumerate(zip(lows, code_starts)):
        if cs is None:
            continue  # whitespace-only trailer (e.g. after the last comma)
        nxt = next((c for c in code_starts[idx + 1 :] if c is not None), body_end)
        pre_ok = any(st == "pre" and lo < s < cs for s, _e, st in comments)
        inline_ok = any(
            st == "inline" and cs < s < nxt and _body_depth(code, body_start, s) == 0
            for s, _e, st in comments
        )
        if pre_ok or inline_ok:
            continue
        line_no = code.count("\n", 0, cs) + 1
        name = member_name(kind, code[cs : highs[idx]])
        label = "enum-value" if kind == "enum" else "member"
        rows.append((rel, line_no, label, name, "undocumented"))
    return rows


_DEFINE_RE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)", re.MULTILINE)


def _preceding_doc_run(
    comments: list[tuple[int, int, str | None]], code: str, upto: int
) -> tuple[list[tuple[int, int]], bool]:
    """Concatenated raw text + has-pre flag for the doc run just above ``upto``.

    Walks the contiguous run of comments immediately preceding ``upto`` (only
    whitespace allowed in the gaps) so both a single ``/** ... */`` block and a
    stack of ``///`` lines are captured. Returns ``(text_spans, has_pre)`` where
    ``text_spans`` is a list of ``(start, end)`` for the run's comments.
    """
    spans = []
    has_pre = False
    boundary = upto
    for start, end, style in reversed(comments):
        if end > boundary:
            continue
        if code[end:boundary].strip():  # code between comment and boundary
            break
        spans.append((start, end))
        has_pre = has_pre or style == "pre"
        boundary = start
    return spans, has_pre


def audit_macros(
    raw: str, code: str, comments: list[tuple[int, int, str | None]], rel: str
) -> list[tuple[str, int, str, str, str]]:
    """Yield offender rows for macros lacking a ``@brief``/``@def`` doc block."""
    rows = []
    for m in _DEFINE_RE.finditer(code):
        name = m.group(1)
        def_line = m.start()
        spans, has_pre = _preceding_doc_run(comments, code, def_line)
        line_no = code.count("\n", 0, def_line) + 1
        if not has_pre:
            rows.append((rel, line_no, "macro", name, "undocumented"))
            continue
        doc_text = "".join(raw[s:e] for s, e in spans)
        if "@brief" not in doc_text and "@def" not in doc_text:
            rows.append((rel, line_no, "macro", name, "missing-brief-or-def"))
    return rows


def audit_members_file(path: Path) -> list[tuple[str, int, str, str, str]]:
    """Return member/enum/macro offender rows for one first-party file."""
    try:
        raw = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    code, comments = blank_noncode(raw)
    try:
        rel = str(path.relative_to(repo_root()))
    except ValueError:  # explicit path outside the repo (e.g. a test fixture)
        rel = str(path)
    rows = []
    for kind, open_idx, close_idx in find_aggregate_bodies(code):
        rows.extend(audit_aggregate(kind, code, comments, open_idx, close_idx, rel))
    rows.extend(audit_macros(raw, code, comments, rel))
    rows.sort(key=lambda r: r[1])
    return rows
