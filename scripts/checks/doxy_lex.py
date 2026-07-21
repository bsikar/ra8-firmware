# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Two ways of looking at C source, and why the auditor needs both.

The function auditor works on comment-STRIPPED source: it is looking for
declarations, and a declaration-shaped line inside a comment is not one.

The member auditor cannot do that -- the documentation it audits *is* the
comments.  So it uses a blanking pass instead: comment and string-literal
interiors become spaces (never removed) and every comment start is recorded
with its Doxygen style.  Because the blanked view is exactly as long as the
raw source, one offset means the same character position in both, and no dual
line/column bookkeeping is needed.

Both passes plus the brace-matching helpers live here so the two views cannot
drift into disagreeing about where a comment ends.
"""

from __future__ import annotations

# Length of a doc-comment introducer ("/**", "/*!", "///", "//!"); the char at
# this offset past the introducer decides inline (`<`) vs preceding style.
DOC_INTRO_LEN = 3


def strip_comments(src: str) -> str:
    """Remove block & line comments without changing line numbers."""
    out = []
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        # preserve string literals
        if c in {'"', "'"}:
            quote = c
            out.append(c)
            i += 1
            while i < n:
                ch = src[i]
                out.append(ch)
                if ch == "\\" and i + 1 < n:
                    out.append(src[i + 1])
                    i += 2
                    continue
                i += 1
                if ch == quote:
                    break
            continue
        if c == "/" and i + 1 < n:
            nxt = src[i + 1]
            if nxt == "/":
                # line comment, keep newlines
                while i < n and src[i] != "\n":
                    i += 1
                continue
            if nxt == "*":
                i += 2
                while i + 1 < n and not (src[i] == "*" and src[i + 1] == "/"):
                    if src[i] == "\n":
                        out.append("\n")
                    i += 1
                i += 2
                continue
        out.append(c)
        i += 1
    return "".join(out)


def find_preceding_doxy(src: str, func_offset: int) -> tuple[str, bool]:
    """Return the doxygen block immediately preceding ``func_offset``.

    Only whitespace may separate the block from the offset; any intervening
    code means the block documents something else and is not returned.

    Returns ``(block_text, True)`` when a block is attached, or ``("", False)``
    when none is -- the flag rather than an empty-string test, so a genuinely
    empty block is still reported as present.
    """
    # walk backward over whitespace
    j = func_offset - 1
    while j >= 0 and src[j] in " \t\n\r":
        j -= 1
    if j < 1 or src[j] != "/" or src[j - 1] != "*":
        return "", False
    # find start of block
    end = j + 1
    k = j - 2
    while k >= 1 and not (src[k] == "/" and src[k + 1] == "*"):
        k -= 1
    if k < 0:
        return "", False
    block = src[k:end]
    # Doxygen blocks start with /** or /*!. Definition-side stub blocks that
    # use /* (single asterisk) and contain "see header for full description"
    # are also accepted as satisfying audit -- they exist purely to mark the
    # function as "documented in header" without producing a duplicate
    # doxygen render.
    if block.startswith(("/**", "/*!")):
        return block, True
    if block.startswith("/*") and (
        "see header for full description" in block
        or "see surrounding code and HUM citations" in block
        or "See the public header for the documented contract" in block
        or "see header for the documented contract" in block
        or "see implementation for details" in block.lower()
    ):
        return block, True
    return "", False


# =============================================================================
# Member / enum-value / macro audit (report-only, issue #246)
# =============================================================================
#
# The function auditor above works on comment-stripped source. The member
# auditor needs to *see* the comments (that is where the documentation lives),
# so it uses a single blanking pass that keeps offsets stable: comment and
# string-literal interiors are replaced by spaces (never removed) and every
# comment start is recorded with its Doxygen style. Because the blanked
# "code-only" view is exactly as long as the raw source, an offset means the
# same character position in both -- no dual-view line/column bookkeeping.


def _block_comment_style(raw: str, i: int) -> str | None:
    """Classify a ``/* ... */`` comment start at offset ``i``.

    Returns "inline" for the Doxygen trailing form (``/**<`` / ``/*!<``),
    "pre" for a preceding doc block (``/**`` / ``/*!``), or None for a plain
    comment. The empty comment ``/**/`` is plain.
    """
    if raw[i : i + 4] == "/**/":
        return None
    if raw[i : i + DOC_INTRO_LEN] in {"/**", "/*!"}:
        after = raw[i + DOC_INTRO_LEN] if i + DOC_INTRO_LEN < len(raw) else ""
        return "inline" if after == "<" else "pre"
    return None


def _line_comment_style(raw: str, i: int) -> str | None:
    """Classify a ``//`` comment start at offset ``i``.

    Returns "inline" for ``///<`` / ``//!<``, "pre" for ``///`` / ``//!``,
    or None for a plain ``//`` comment.
    """
    if raw[i : i + DOC_INTRO_LEN] in {"///", "//!"}:
        after = raw[i + DOC_INTRO_LEN] if i + DOC_INTRO_LEN < len(raw) else ""
        return "inline" if after == "<" else "pre"
    return None


def blank_noncode(raw: str) -> tuple[str, list[tuple[int, int, str | None]]]:  # noqa: PLR0912, PLR0915  # char scanner, splitting hurts clarity
    """Blank comment and string interiors to spaces, keeping offsets stable.

    Returns ``(codeonly, comments)`` where ``codeonly`` is the same length as
    ``raw`` (newlines preserved) with every comment and string-literal interior
    replaced by spaces, and ``comments`` is a list of ``(start, end, style)``
    tuples (style in {"pre", "inline", None}) for each comment, in source order.
    """
    out = list(raw)
    comments = []
    i = 0
    n = len(raw)
    while i < n:
        c = raw[i]
        if c in {'"', "'"}:
            quote = c
            i += 1
            while i < n:
                ch = raw[i]
                if ch == "\\" and i + 1 < n:
                    out[i] = " "
                    out[i + 1] = "\n" if raw[i + 1] == "\n" else " "
                    i += 2
                    continue
                if ch == quote:
                    break
                if ch != "\n":
                    out[i] = " "
                i += 1
            i += 1
            continue
        if c == "/" and i + 1 < n and raw[i + 1] == "/":
            style = _line_comment_style(raw, i)
            start = i
            while i < n and raw[i] != "\n":
                out[i] = " "
                i += 1
            comments.append((start, i, style))
            continue
        if c == "/" and i + 1 < n and raw[i + 1] == "*":
            style = _block_comment_style(raw, i)
            start = i
            out[i] = " "
            out[i + 1] = " "
            i += 2
            while i + 1 < n and not (raw[i] == "*" and raw[i + 1] == "/"):
                if raw[i] != "\n":
                    out[i] = " "
                i += 1
            if i + 1 < n:
                out[i] = " "
                out[i + 1] = " "
                i += 2
            else:  # unterminated block comment: blank the tail
                while i < n:
                    if raw[i] != "\n":
                        out[i] = " "
                    i += 1
            comments.append((start, i, style))
            continue
        i += 1
    return "".join(out), comments


def _match_brace(code: str, open_idx: int) -> int | None:
    """Offset of the ``}`` matching the ``{`` at ``open_idx``, or None if unbalanced.

    Returns None rather than raising on an unterminated brace: this runs over
    partially-valid source, and an unbalanced file should be skipped, not
    crash the sweep.
    """
    depth = 0
    i = open_idx
    n = len(code)
    while i < n:
        ch = code[i]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return None


def _first_code_offset(code: str, lo: int, hi: int) -> int | None:
    """First non-whitespace offset in the exclusive range ``(lo, hi)``, else None.

    Starts at ``lo + 1``, so the delimiter at ``lo`` is never itself returned.
    """
    i = lo + 1
    while i < hi:
        if not code[i].isspace():
            return i
        i += 1
    return None


def _body_depth(code: str, body_start: int, pos: int) -> int:
    """Brace depth of ``pos`` relative to a body opening at ``body_start``.

    0 means directly inside the aggregate body (not within a nested
    struct/union). Comment/string braces are already blanked in ``code``.
    """
    return code.count("{", body_start + 1, pos) - code.count("}", body_start + 1, pos)
