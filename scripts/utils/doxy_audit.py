#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Doxygen documentation gap auditor for ra8-firmware.

Walks every .c/.h under libs/, src/, port/ (excluding libs/third_party/),
locates every function definition/prototype, and checks the immediately
preceding Doxygen block for the required tags listed in CLAUDE.md
("Doxygen Documentation Requirements").

Modes
-----
  (no args)         Function audit report -> docs/DOXYGEN_GAPS.csv + .md
  --check           Strict function gate (exit 1 on any gap). Wired into CI
                    and the pre-commit hook.
  --members         REPORT-ONLY member audit. Enumerates every undocumented
                    enum value, struct/union member, and macro across the
                    first-party tree and prints an offender count per top-level
                    dir. This mode never fails the build. Optionally takes
                    explicit file paths to audit just those, and --out=PATH to
                    dump the full offender list as CSV.
  --members --check ENFORCING member gate (exit 1 on any undocumented enum
                    value, struct/union member, or macro). Wired into CI and
                    the pre-commit hook alongside the function gate (issue
                    #246). Optionally takes explicit file paths to gate just
                    those.

CLAUDE.md ("Doxygen Documentation Requirements") demands that *every* enum
value, struct/union member, and macro carry documentation -- an inline
``/**< ... */`` (or a preceding ``/** ... */`` block) on each aggregate member
and a ``@brief``/``@def`` block on each macro. ``--members --check`` enforces
exactly that (issue #246); plain ``--members`` is the sizing report.

Output:
  - docs/DOXYGEN_GAPS.csv  full row-per-function table
  - docs/DOXYGEN_GAPS.md   human-readable summary
  - (--members) stdout summary; optional --out=PATH member-gap CSV

Audit-only: never edits source files.
"""

from __future__ import annotations

import os
import re
import sys
from collections import Counter
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCAN_DIRS = ["libs", "src", "port"]
EXCLUDE_PARTS = {"third_party", "build", ".git"}

# Top-level dirs scanned by the report-only member audit (--members). Wider
# than SCAN_DIRS on purpose: the member/enum/macro documentation bar is
# repo-wide (CLAUDE.md "these standards apply to EVERY first-party file"), so
# the fallout report must cover examples/, tools/, and tests/ too. Vendored
# SOUP (libs/third_party/) and generated tables (libs/fonts/) stay exempt, the
# same as the function audit.
MEMBER_SCAN_DIRS = ["libs", "src", "port", "examples", "tools", "tests"]
MEMBER_EXCLUDE_PARTS = {"third_party", "fonts", "build", "build-cov", "_deps", ".git"}

# Length of a doc-comment introducer ("/**", "/*!", "///", "//!"); the char at
# this offset past the introducer decides inline (`<`) vs preceding style.
DOC_INTRO_LEN = 3

# Required tags for every function (per CLAUDE.md)
REQUIRED_SCALAR = ["@brief", "@details", "@return", "@retval", "@note", "@since"]
# @pre and @post require a minimum of 2 each (NASA Rule 5)
REQUIRED_MIN2 = ["@pre", "@post"]

# NASA Power of 10 Rule 5 mandates at least 2 preconditions and 2 postconditions.
NASA_RULE5_MIN_PRE_POST = 2

# Minimum path depth to form a two-segment module label (e.g. "libs/ra8_hal").
MODULE_PATH_MIN_DEPTH = 2

# Maximum number of lines in the generated Markdown report (keep pre-commit output brief).
MD_REPORT_LINE_CAP = 200

FUNC_RE = re.compile(
    # return-type tokens (allow pointers, qualifiers, attributes)
    r"^[ \t]*"
    r"(?P<ret>(?:(?:static|inline|extern|const|volatile|register|signed|unsigned|"
    r"struct|union|enum|__attribute__\s*\(\([^)]*\)\)|[A-Za-z_][A-Za-z_0-9]*)\s+|\*\s*)+)"
    r"(?P<name>[A-Za-z_][A-Za-z_0-9]*)\s*"
    r"\((?P<args>[^;{]*)\)\s*"
    r"(?:__attribute__\s*\(\([^)]*\)\)\s*)?"
    r"(?P<term>[{;])",
    re.MULTILINE,
)

# Things that look like function decls but aren't
NON_FUNC_NAMES = {
    "if",
    "for",
    "while",
    "switch",
    "return",
    "sizeof",
    "typeof",
    "do",
    "else",
    "case",
    "goto",
    "static_assert",
    "_Static_assert",
    "alignof",
    "_Alignof",
    "defined",
    # Inline-asm misparse: `__asm__ volatile("...")` looks like a function
    # named `volatile` to the regex; it has no real prototype to document.
    "volatile",
}


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


def find_preceding_doxy(src: str, func_offset: int):
    """Return (block_text, has_block) for the doxygen block immediately
    preceding func_offset, or ("", False) if none."""
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


def parse_args(args_text: str):  # noqa: PLR0912  # parameter-parsing dispatch, splitting hurts readability
    """Return list of parameter names. (void) -> []."""
    s = args_text.strip()
    if s in {"", "void"}:
        return []
    # strip nested attributes
    s = re.sub(r"__attribute__\s*\(\([^)]*\)\)", "", s)
    params = []
    depth = 0
    cur = []
    for ch in s:
        if ch in {"(", "[", "{"}:
            depth += 1
            cur.append(ch)
        elif ch in {")", "]", "}"}:
            depth -= 1
            cur.append(ch)
        elif ch == "," and depth == 0:
            params.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    if cur:
        params.append("".join(cur).strip())

    names = []
    for p in params:
        if not p or p == "void":
            continue
        if p == "...":
            continue
        # remove default-ish noise; strip array brackets
        p2 = re.sub(r"\[[^\]]*\]", "", p).strip()
        # function pointer: extract (*name)
        fp = re.search(r"\(\s*\*\s*([A-Za-z_][A-Za-z_0-9]*)\s*\)", p2)
        if fp:
            names.append(fp.group(1))
            continue
        # last identifier token is the name
        toks = re.findall(r"[A-Za-z_][A-Za-z_0-9]*", p2)
        if not toks:
            continue
        # filter out type keywords if it's the only one
        if len(toks) == 1 and toks[0] in {
            "int",
            "char",
            "short",
            "long",
            "float",
            "double",
            "void",
            "signed",
            "unsigned",
            "bool",
            "size_t",
            "ssize_t",
        }:
            # unnamed parameter, count as positional
            names.append(f"arg{len(names)}")
            continue
        names.append(toks[-1])
    return names


def is_returning_void(ret: str) -> bool:
    r = ret.strip()
    # collapse whitespace
    r = re.sub(r"\s+", " ", r)
    # strip qualifiers
    r = re.sub(r"\b(static|inline|extern|__attribute__\(\([^)]*\)\))\b", "", r).strip()
    # pointer return is not void
    if "*" in r:
        return False
    # exact "void"
    return r == "void" or r.endswith(" void")


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


def blank_noncode(raw: str):  # noqa: PLR0912, PLR0915  # char scanner, splitting hurts clarity
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


def _match_brace(code: str, open_idx: int):
    """Return the offset of the ``}`` matching the ``{`` at ``open_idx``."""
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


_AGG_KW_RE = re.compile(r"\b(enum|struct|union)\b")
_AGG_TERMINATORS = frozenset(";=,}")


def find_aggregate_bodies(code: str):  # noqa: PLR0912  # char scanner; a helper per branch hurts clarity
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


def _first_code_offset(code: str, lo: int, hi: int):
    """First non-whitespace offset in the half-open range ``(lo, hi)``."""
    i = lo + 1
    while i < hi:
        if not code[i].isspace():
            return i
        i += 1
    return None


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


def _body_depth(code: str, body_start: int, pos: int) -> int:
    """Brace depth of ``pos`` relative to a body opening at ``body_start``.

    0 means directly inside the aggregate body (not within a nested
    struct/union). Comment/string braces are already blanked in ``code``.
    """
    return code.count("{", body_start + 1, pos) - code.count("}", body_start + 1, pos)


def audit_aggregate(kind, code, comments, open_idx, close_idx, rel):  # noqa: PLR0913  # body slice
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


def _preceding_doc_run(comments, code, upto):
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


def audit_macros(raw, code, comments, rel):
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


def audit_members_file(path: Path):
    """Return member/enum/macro offender rows for one first-party file."""
    try:
        raw = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    code, comments = blank_noncode(raw)
    try:
        rel = str(path.relative_to(REPO_ROOT))
    except ValueError:  # explicit path outside the repo (e.g. a test fixture)
        rel = str(path)
    rows = []
    for kind, open_idx, close_idx in find_aggregate_bodies(code):
        rows.extend(audit_aggregate(kind, code, comments, open_idx, close_idx, rel))
    rows.extend(audit_macros(raw, code, comments, rel))
    rows.sort(key=lambda r: r[1])
    return rows


def definition_carries_block(raw: str, src_no_comments: str, name: str) -> bool:
    """True when a *definition* of ``name`` in this same file carries a block.

    Used to tell a file-local forward prototype apart from a genuinely
    undocumented function.  See the call site for why that distinction has to
    exist.
    """
    raw_lines = raw.splitlines(keepends=True)
    for dm in FUNC_RE.finditer(src_no_comments):
        if dm.group("name") != name or dm.group("term") != "{":
            continue
        line_no = src_no_comments.count("\n", 0, dm.start()) + 1
        if line_no - 1 >= len(raw_lines):
            continue
        offset = sum(len(ln) for ln in raw_lines[: line_no - 1])
        _, has_block = find_preceding_doxy(raw, offset)
        if has_block:
            return True
    return False


def audit_file(path: Path):  # noqa: PLR0912 PLR0915  # audit-dispatch, splitting hurts readability
    try:
        raw = path.read_text(encoding="utf-8", errors="replace")
    except Exception:
        return []
    src_no_comments = strip_comments(raw)

    rows = []
    for m in FUNC_RE.finditer(src_no_comments):
        name = m.group("name")
        if name in NON_FUNC_NAMES:
            continue
        ret = m.group("ret")
        args = m.group("args")
        # filter out things like "return foo(x);" patterns
        if "return" in ret.split():
            continue
        # filter typedef/struct lines
        if re.search(r"\btypedef\b", ret):
            continue
        # Reject call expressions misparsed as declarations.
        # 1) An assignment target on the LHS: the args group spilled past the
        #    real closing paren and now contains '=' or an extra ')'. This is
        #    the `*foo(off) = bar;` / `*foo() = ...;` shape used heavily by
        #    inline-accessor register writes.
        if "=" in args or ")" in args:
            continue
        # 2) Return-type token is *only* a dereference (`*` or `&`) with no
        #    real type name -- those are call expressions, not declarations.
        ret_stripped = ret.strip()
        if ret_stripped in ("*", "&") or re.fullmatch(r"[*&\s]+", ret_stripped):
            continue
        # 3) Full matched text starting with `*name(` or `&name(` is a call
        #    expression (deref/address-of of an inline accessor return).
        full = m.group(0).lstrip()
        if full.startswith(("*", "&")):
            continue
        # skip definitions of macros (shouldn't appear since stripped, but safety)
        # determine the line number in original file
        # m.start() is an offset into src_no_comments; since strip_comments
        # preserves newlines, line numbers in stripped == line numbers in raw.
        line_no = src_no_comments.count("\n", 0, m.start()) + 1

        args = parse_args(args)

        # Get the doxy block from the *original* source so comments are present
        # We need the offset in raw. Use line_no to approximate.
        # Find the function start in raw by line-based search.
        raw_lines = raw.splitlines(keepends=True)
        if line_no - 1 >= len(raw_lines):
            continue
        offset_in_raw = sum(len(ln) for ln in raw_lines[: line_no - 1])
        block, has_block = find_preceding_doxy(raw, offset_in_raw)

        # Definition-site policy (CLAUDE.md "Definition-site comments"): the
        # authoritative Doxygen block lives on the function's *declaration* in
        # the header, which this tool audits when it scans that .h. A
        # non-static definition in a .c therefore needs no block of its own --
        # the default is no comment, and a restating block only rots. A
        # static (TU-local) function has no header to carry the contract, so
        # it still requires a full definition-site block.
        if str(path).endswith(".c") and not re.search(r"\bstatic\b", ret):
            rows.append((str(path.relative_to(REPO_ROOT)), line_no, name, [], "ok"))
            continue

        # A file-local forward prototype exists so a function can be *called*
        # before it is defined (``static void f(void);`` near the top, body 600
        # lines down). It is an ordering device, not a second contract. When
        # the definition in this same file carries the block, demanding one
        # here too would force the identical block to be written twice -- and a
        # duplicated block is precisely the defect check_doc_attachment.py
        # rejects as DOC004/DOC006. Without this, the two gates contradict each
        # other and no source file can satisfy both at once (ra8_rsip.c's
        # internal_sw_sha256 is the live example). A prototype whose definition
        # is *also* bare is still reported, at the definition.
        if (
            m.group("term") == ";"
            and not has_block
            and re.search(r"\bstatic\b", ret)
            and definition_carries_block(raw, src_no_comments, name)
        ):
            rows.append((str(path.relative_to(REPO_ROOT)), line_no, name, [], "ok"))
            continue

        missing = []

        if not has_block:
            # Treat as everything missing
            missing.append("@brief")
            missing.append("@details")
            missing.extend(f"@param[{a}]" for a in args)
            if not is_returning_void(ret):
                missing.append("@return")
                missing.append("@retval")
            missing.append("@pre")
            missing.append("@post")
            missing.append("@note")
            missing.append("@since")
        else:
            # Stub block ('/* ... see header for full description ... */')
            # marks the function as documented in its declaring header. The
            # canonical tags live there; the .c stub deliberately uses a
            # single asterisk so doxygen ignores the block (silencing
            # "multiple @param documentation sections" duplication warnings).
            if (
                block.startswith("/*")
                and not block.startswith("/**")
                and (
                    "see header for full description" in block
                    or "see surrounding code and HUM citations" in block
                    or "See the public header for the documented contract" in block
                    or "see header for the documented contract" in block
                    or "see implementation for details" in block.lower()
                )
            ):
                rows.append((str(path.relative_to(REPO_ROOT)), line_no, name, [], "ok"))
                continue
            # @copydoc / @copydetails / @copybrief satisfy every required
            # documentation tag because doxygen literally substitutes the
            # source block at render time. Used on definition-side blocks
            # whose declaration in a header carries the canonical doc and
            # which would otherwise duplicate every @param / @retval entry,
            # producing a flood of "multiple @param documentation sections"
            # warnings in the generated HTML.
            if re.search(r"@copy(doc|details|brief)\b", block):
                rows.append((str(path.relative_to(REPO_ROOT)), line_no, name, [], "ok"))
                continue
            # @brief
            if "@brief" not in block:
                missing.append("@brief")
            if "@details" not in block:
                missing.append("@details")
            # @param[in/out/in,out] <name>
            for a in args:
                # match @param[...] name OR @param name (any direction)
                pat = re.compile(r"@param(?:\s*\[[^\]]*\])?\s+" + re.escape(a) + r"\b")
                if not pat.search(block):
                    missing.append(f"@param[{a}]")
            # @return / @retval (only if non-void)
            if not is_returning_void(ret):
                if "@return" not in block and "@returns" not in block:
                    missing.append("@return")
                if "@retval" not in block:
                    missing.append("@retval")
            # @pre/@post require >=2
            n_pre = len(re.findall(r"@pre\b", block))
            n_post = len(re.findall(r"@post\b", block))
            if n_pre < NASA_RULE5_MIN_PRE_POST:
                missing.append(f"@pre(<2:{n_pre})")
            if n_post < NASA_RULE5_MIN_PRE_POST:
                missing.append(f"@post(<2:{n_post})")
            if "@note" not in block:
                missing.append("@note")
            if "@since" not in block:
                missing.append("@since")

        if not missing:
            rows.append((str(path.relative_to(REPO_ROOT)), line_no, name, [], "ok"))
            continue

        # severity
        has_brief_or_param_miss = any(t == "@brief" or t.startswith("@param[") for t in missing)
        if has_brief_or_param_miss:
            severity = "high"
        elif any(t in ("@return", "@retval") or t.startswith(("@pre", "@post")) for t in missing):
            severity = "medium"
        else:
            severity = "low"

        rows.append((str(path.relative_to(REPO_ROOT)), line_no, name, missing, severity))

    return rows


def run_check() -> int:
    """Strict gate: exit 0 if zero gaps, else exit 1 with offender list.

    Used by the pre-commit hook to keep documentation coverage at 100%.
    Does not write CSV/MD outputs -- read-only audit.
    """
    all_rows = []
    for top in SCAN_DIRS:
        root = REPO_ROOT / top
        if not root.is_dir():
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in EXCLUDE_PARTS]
            for fn in filenames:
                if not (fn.endswith((".c", ".h"))):
                    continue
                p = Path(dirpath) / fn
                rel_parts = p.relative_to(REPO_ROOT).parts
                if any(part in EXCLUDE_PARTS for part in rel_parts):
                    continue
                all_rows.extend(audit_file(p))

    gap_rows = [r for r in all_rows if r[3]]
    if not gap_rows:
        print("doxy_audit --check: gaps=0 (PASS)")
        return 0

    print(f"doxy_audit --check: gaps={len(gap_rows)} (FAIL)")
    print("Offending functions (file:line  function  -- missing tags):")
    # cap output to 50 lines so the hook stays readable
    cap = 50
    for src, line, name, missing, _sev in gap_rows[:cap]:
        print(f"  {src}:{line}  {name}  --  {';'.join(missing)}")
    if len(gap_rows) > cap:
        print(f"  ... and {len(gap_rows) - cap} more")
    print()
    print("Refresh the audit report by running:")
    print("  python3 scripts/utils/doxy_audit.py")
    return 1


def _iter_member_files(explicit):
    """Yield first-party .c/.h paths for the member audit.

    ``explicit`` is a list of user-supplied paths; if non-empty those exact
    files are audited, otherwise MEMBER_SCAN_DIRS is walked.
    """
    if explicit:
        for raw_path in explicit:
            p = Path(raw_path)
            if p.is_file():  # relative to CWD or absolute
                yield p
            elif (REPO_ROOT / raw_path).is_file():  # relative to the repo root
                yield REPO_ROOT / raw_path
        return
    for top in MEMBER_SCAN_DIRS:
        root = REPO_ROOT / top
        if not root.is_dir():
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in MEMBER_EXCLUDE_PARTS]
            for fn in filenames:
                if not fn.endswith((".c", ".h")):
                    continue
                p = Path(dirpath) / fn
                if any(part in MEMBER_EXCLUDE_PARTS for part in p.relative_to(REPO_ROOT).parts):
                    continue
                yield p


def _top_dir(rel: str) -> str:
    """Top-level directory of a repo-relative path (e.g. "libs")."""
    return rel.split("/", 1)[0]


def run_members_report(explicit, out_csv) -> int:
    """REPORT-ONLY member/enum/macro audit. Always returns 0.

    Enumerates every undocumented enum value, struct/union member, and macro
    across the first-party tree and prints an offender count per top-level dir
    so the owner can size the fallout wave (issue #246). This mode intentionally
    never fails: promoting the member checks to a hard gate is a follow-up.
    """
    all_rows = []
    for p in _iter_member_files(explicit):
        all_rows.extend(audit_members_file(p))

    by_dir_kind = Counter((_top_dir(r[0]), r[2]) for r in all_rows)
    by_dir = Counter(_top_dir(r[0]) for r in all_rows)
    by_kind = Counter(r[2] for r in all_rows)
    by_file = Counter(r[0] for r in all_rows)

    if out_csv is not None:
        out_path = Path(out_csv)
        if not out_path.is_absolute():
            out_path = REPO_ROOT / out_path
        with out_path.open("w", encoding="ascii") as f:
            f.write("source_file,line,element,name,reason\n")
            for rel, line, kind, name, reason in sorted(all_rows, key=lambda r: (r[0], r[1])):
                f.write(f"{rel},{line},{kind},{name},{reason}\n")

    kinds = ["enum-value", "member", "macro"]
    dirs = sorted(by_dir)

    print("doxy_audit --members: REPORT-ONLY member/enum/macro documentation audit")
    print("(this mode never fails the build; see issue #246)")
    print()
    print("Undocumented offenders per top-level dir:")
    header = f"  {'dir':<12}" + "".join(f"{k:>13}" for k in kinds) + f"{'total':>10}"
    print(header)
    print("  " + "-" * (len(header) - 2))
    for d in dirs:
        cells = "".join(f"{by_dir_kind[(d, k)]:>13}" for k in kinds)
        print(f"  {d:<12}{cells}{by_dir[d]:>10}")
    totals = "".join(f"{by_kind[k]:>13}" for k in kinds)
    print("  " + "-" * (len(header) - 2))
    print(f"  {'TOTAL':<12}{totals}{len(all_rows):>10}")
    print()
    print(f"Files scanned with at least one offender: {len(by_file)}")
    print("Worst 15 files by offender count:")
    for rel, cnt in by_file.most_common(15):
        print(f"  {cnt:>6}  {rel}")
    if out_csv is not None:
        print()
        print(f"Full offender list written to: {out_csv}")
    return 0


def run_members_check(explicit) -> int:
    """ENFORCING member gate: exit 0 if zero offenders, else exit 1.

    Enforces the CLAUDE.md rule that every enum value, struct/union member, and
    macro carries documentation (issue #246). Wired into the pre-commit hook and
    CI alongside the function gate. Read-only -- writes no CSV/MD outputs.
    """
    all_rows = []
    for p in _iter_member_files(explicit):
        all_rows.extend(audit_members_file(p))
    all_rows.sort(key=lambda r: (r[0], r[1]))

    if not all_rows:
        print("doxy_audit --members --check: offenders=0 (PASS)")
        return 0

    by_kind = Counter(r[2] for r in all_rows)
    print(f"doxy_audit --members --check: offenders={len(all_rows)} (FAIL)")
    print(
        "  by kind: "
        + ", ".join(f"{k}={by_kind[k]}" for k in ("enum-value", "member", "macro") if by_kind[k])
    )
    print("Undocumented members (file:line  kind  name  -- reason):")
    cap = 50
    for rel, line, kind, name, reason in all_rows[:cap]:
        print(f"  {rel}:{line}  {kind}  {name}  --  {reason}")
    if len(all_rows) > cap:
        print(f"  ... and {len(all_rows) - cap} more")
    print()
    print("Every enum value, struct/union member, and macro needs a doc comment:")
    print("  - aggregate members: an inline /**< ... */ or a preceding /** ... */ block")
    print("  - macros: a preceding /** @brief ... */ (or @def) block")
    print("Size the full fallout with: python3 scripts/utils/doxy_audit.py --members")
    return 1


def _parse_members_args(argv):
    """Split --members argv into (explicit_paths, out_csv)."""
    explicit = []
    out_csv = None
    for a in argv:
        if a == "--members":
            continue
        if a.startswith("--out="):
            out_csv = a[len("--out=") :]
        elif not a.startswith("--"):
            explicit.append(a)
    return explicit, out_csv


def main() -> int:  # noqa: PLR0912 PLR0915  # report-generation dispatch, splitting hurts readability
    if "--members" in sys.argv[1:]:
        explicit, out_csv = _parse_members_args(sys.argv[1:])
        if "--check" in sys.argv[1:]:
            return run_members_check(explicit)
        return run_members_report(explicit, out_csv)
    if "--check" in sys.argv[1:]:
        return run_check()

    all_rows = []
    for top in SCAN_DIRS:
        root = REPO_ROOT / top
        if not root.is_dir():
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            # prune excluded dirs
            dirnames[:] = [d for d in dirnames if d not in EXCLUDE_PARTS]
            for fn in filenames:
                if not (fn.endswith((".c", ".h"))):
                    continue
                p = Path(dirpath) / fn
                rel_parts = p.relative_to(REPO_ROOT).parts
                if any(part in EXCLUDE_PARTS for part in rel_parts):
                    continue
                all_rows.extend(audit_file(p))

    # sort by file path then line
    all_rows.sort(key=lambda r: (r[0], r[1]))

    total_funcs = len(all_rows)
    gap_rows = [r for r in all_rows if r[3]]
    total_gaps = len(gap_rows)
    total_missing_tags = sum(len(r[3]) for r in gap_rows)

    # write CSV
    csv_path = REPO_ROOT / "docs" / "DOXYGEN_GAPS.csv"
    with csv_path.open("w", encoding="ascii") as f:
        f.write("source_file,line,function_name,missing_tags,severity\n")
        for src, line, name, missing, sev in gap_rows:
            f.write(f"{src},{line},{name},{';'.join(missing)},{sev}\n")

    # tag frequency (collapse @param[*] -> @param, @pre(<2:n) -> @pre, etc.)
    tag_counter = Counter()
    for r in gap_rows:
        for t in r[3]:
            if t.startswith("@param["):
                tag_counter["@param"] += 1
            elif t.startswith("@pre"):
                tag_counter["@pre"] += 1
            elif t.startswith("@post"):
                tag_counter["@post"] += 1
            else:
                tag_counter[t] += 1

    # per-file gap counts
    file_counter = Counter(r[0] for r in gap_rows)

    # per-module (top-level dir within libs/, src/, port/)
    def module_of(path: str) -> str:
        parts = path.split("/")
        if len(parts) >= MODULE_PATH_MIN_DEPTH:
            return f"{parts[0]}/{parts[1]}"
        return parts[0]

    module_counter = Counter(module_of(r[0]) for r in gap_rows)

    # write Markdown
    md_path = REPO_ROOT / "docs" / "DOXYGEN_GAPS.md"
    lines = []
    lines.append("# Doxygen Documentation Gap Report")
    lines.append("")
    lines.append("Audit-only report generated by `scripts/utils/doxy_audit.py` against the")
    lines.append("Doxygen Documentation Requirements in `CLAUDE.md`. Scope: `libs/`,")
    lines.append("`src/`, `port/` (excludes `libs/third_party/`).")
    lines.append("")
    lines.append("## Summary")
    lines.append("")
    lines.append(f"- Total functions audited: {total_funcs}")
    lines.append(f"- Functions with gaps: {total_gaps}")
    lines.append(f"- Total missing-tag instances: {total_missing_tags}")
    lines.append("")
    lines.append("## Most-frequently-missing tags")
    lines.append("")
    lines.append("| Tag | Count |")
    lines.append("|-----|-------|")
    for tag, cnt in tag_counter.most_common():
        lines.append(f"| `{tag}` | {cnt} |")
    lines.append("")
    lines.append("## Worst 10 modules by gap count")
    lines.append("")
    lines.append("| Module | Functions with gaps |")
    lines.append("|--------|---------------------|")
    for mod, cnt in module_counter.most_common(10):
        lines.append(f"| `{mod}` | {cnt} |")
    lines.append("")
    lines.append("## Top 30 files by gap count")
    lines.append("")
    lines.append("| File | Functions with gaps |")
    lines.append("|------|---------------------|")
    for fn, cnt in file_counter.most_common(30):
        lines.append(f"| `{fn}` | {cnt} |")
    lines.append("")
    lines.append("## Severity legend")
    lines.append("")
    lines.append("- `high`: missing `@brief` or any `@param[...]`")
    lines.append("- `medium`: missing `@return` / `@retval` / `@pre` / `@post`")
    lines.append("- `low`: only optional / informational tags missing (`@note`, `@since`, ...)")
    lines.append("")
    lines.append("See `docs/DOXYGEN_GAPS.csv` for the full row-by-row data.")
    lines.append("")
    lines.append("## Audit history")
    lines.append("")
    lines.append("| Date | Functions with gaps | Missing-tag instances |")
    lines.append("|------|---------------------|-----------------------|")
    lines.append("| 2026-05-02 (original)                 | 2557 | 20328 |")
    lines.append("| 2026-05-02 (refresh)                  | 663  | 4935  |")
    lines.append(f"| 2026-05-02 (auditor false-pos fix)    | {total_gaps} | {total_missing_tags} |")
    lines.append("")

    # cap at MD_REPORT_LINE_CAP lines to keep the report browsable
    if len(lines) > MD_REPORT_LINE_CAP:
        lines = lines[:MD_REPORT_LINE_CAP]

    md_path.write_text("\n".join(lines), encoding="ascii")

    print(f"functions={total_funcs} gaps={total_gaps} missing_tags={total_missing_tags}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
