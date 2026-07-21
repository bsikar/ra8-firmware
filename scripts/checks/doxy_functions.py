# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""The function gate: every function carries the tags CLAUDE.md requires.

Recognising a function is most of the work, and it is done with a regex over
comment-stripped source rather than a parse.  That is a deliberate trade --
see the ``doxy_audit`` docstring on why this stayed regex-driven while
``check_doc_attachment.py`` took the libclang dependency -- and it is why
:func:`_is_declaration` exists: the regex over-matches call expressions, and
each rejection below corresponds to a real construct in this tree that was
once audited as if it were a function.

The waivers in :func:`_waiver_row` are equally load-bearing. Two of them exist
because this gate and ``check_doc_attachment.py`` would otherwise contradict
each other -- demanding a block here that the other rejects as a duplicate --
and no source file could satisfy both at once.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

from doxy_lex import find_preceding_doxy, strip_comments
from doxy_scope import repo_root

# Required tags for every function (per CLAUDE.md)
REQUIRED_SCALAR = ["@brief", "@details", "@return", "@retval", "@note", "@since"]
# @pre and @post require a minimum of 2 each (NASA Rule 5)
REQUIRED_MIN2 = ["@pre", "@post"]

# NASA Power of 10 Rule 5 mandates at least 2 preconditions and 2 postconditions.
NASA_RULE5_MIN_PRE_POST = 2

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

#: Comment texts that mark a definition as documented in its declaring header.
#: The .c stub deliberately uses a single asterisk so doxygen ignores the
#: block, silencing "multiple @param documentation sections" warnings.
_HEADER_STUB_MARKERS = (
    "see header for full description",
    "see surrounding code and HUM citations",
    "See the public header for the documented contract",
    "see header for the documented contract",
)

#: @copydoc / @copydetails / @copybrief satisfy every required tag: doxygen
#: literally substitutes the source block at render time, so restating the
#: tags here would duplicate every @param / @retval in the generated HTML.
_COPY_RE = re.compile(r"@copy(doc|details|brief)\b")


#: Bare type keywords. A parameter whose only identifier is one of these is
#: unnamed (`void f(int)`), so it gets a positional stand-in rather than
#: being documented under the name of its own type.
_BARE_TYPE_KEYWORDS = frozenset(
    {
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
    }
)


def _split_params(args_text: str) -> list[str]:
    """Split a parameter list on top-level commas only.

    Depth-aware because a function-pointer parameter or a nested initialiser
    carries commas of its own -- `void f(int (*cb)(int, int), size_t n)` is
    two parameters, not three.
    """
    s = args_text.strip()
    if s in {"", "void"}:
        return []
    s = re.sub(r"__attribute__\s*\(\([^)]*\)\)", "", s)
    params: list[str] = []
    depth = 0
    cur: list[str] = []
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
    return params


def _param_name(param: str, position: int) -> str | None:
    """Return the documented name of one parameter, or None when it has none."""
    if not param or param in {"void", "..."}:
        return None
    stripped = re.sub(r"\[[^\]]*\]", "", param).strip()
    # Function pointer: the name sits inside the (*name) group.
    fp = re.search(r"\(\s*\*\s*([A-Za-z_][A-Za-z_0-9]*)\s*\)", stripped)
    if fp:
        return fp.group(1)
    toks = re.findall(r"[A-Za-z_][A-Za-z_0-9]*", stripped)
    if not toks:
        return None
    if len(toks) == 1 and toks[0] in _BARE_TYPE_KEYWORDS:
        return f"arg{position}"
    return toks[-1]


def parse_args(args_text: str) -> list[str]:
    """Return the list of parameter names. ``(void)`` -> ``[]``."""
    names: list[str] = []
    for param in _split_params(args_text):
        name = _param_name(param, len(names))
        if name is not None:
            names.append(name)
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


def _is_declaration(m: re.Match) -> bool:
    """Reject the call expressions ``FUNC_RE`` also matches.

    Each rejection below is a real shape from this tree, not a hypothetical:
    the ``*foo(off) = bar;`` inline-accessor register write, a `return f(x);`
    statement, and a bare dereference of an accessor's return value all match
    a regex looking for `<tokens> name(args) {` or `;`.
    """
    if m.group("name") in NON_FUNC_NAMES:
        return False
    ret, args = m.group("ret"), m.group("args")
    if "return" in ret.split() or re.search(r"\btypedef\b", ret):
        return False
    # An assignment target on the LHS: the args group spilled past the real
    # closing paren and now holds '=' or an extra ')'.
    if "=" in args or ")" in args:
        return False
    # A return type that is *only* a dereference is a call expression.
    ret_stripped = ret.strip()
    if ret_stripped in ("*", "&") or re.fullmatch(r"[*&\s]+", ret_stripped):
        return False
    # Matched text starting `*name(` / `&name(` -- deref of an accessor.
    return not m.group(0).lstrip().startswith(("*", "&"))


@dataclass(frozen=True)
class _SourceViews:
    """The one file under audit, in the two views the waivers need.

    ``raw`` still has its comments (that is where a deferring block lives);
    ``stripped`` has them blanked (that is where a definition is found).
    """

    path: Path
    raw: str
    stripped: str


def _is_waived(views: _SourceViews, m: re.Match, block: str, *, has_block: bool) -> bool:
    """True when this declaration legitimately needs no block of its own.

    Three waivers, each closing a contradiction rather than lowering the bar:

    * A non-static definition in a ``.c``. CLAUDE.md ("Definition-site
      comments") puts the authoritative block on the *declaration* in the
      header, which this tool audits when it scans that ``.h``. A restating
      block at the definition only rots. A ``static`` function has no header
      to carry the contract, so it still needs a full block.
    * A file-local forward prototype whose definition below carries the block.
      The prototype is an ordering device, not a second contract, and
      demanding a block on both is precisely the duplication
      ``check_doc_attachment.py`` rejects as DOC004/DOC006 -- without this the
      two gates contradict each other and no file can satisfy both
      (``ra8_rsip.c``'s ``internal_sw_sha256`` is the live example). A
      prototype whose definition is ALSO bare is still reported, at the
      definition.
    * A block that defers to the header, or substitutes one with ``@copydoc``.
    """
    ret = m.group("ret")
    if str(views.path).endswith(".c") and not re.search(r"\bstatic\b", ret):
        return True
    if (
        m.group("term") == ";"
        and not has_block
        and re.search(r"\bstatic\b", ret)
        and definition_carries_block(views.raw, views.stripped, m.group("name"))
    ):
        return True
    if not has_block:
        return False
    is_header_stub = (
        block.startswith("/*")
        and not block.startswith("/**")
        and (
            any(marker in block for marker in _HEADER_STUB_MARKERS)
            or "see implementation for details" in block.lower()
        )
    )
    return is_header_stub or bool(_COPY_RE.search(block))


def _missing_without_block(args: list[str], ret: str) -> list[str]:
    """Every required tag, for a function carrying no block at all."""
    missing = ["@brief", "@details", *(f"@param[{a}]" for a in args)]
    if not is_returning_void(ret):
        missing += ["@return", "@retval"]
    return [*missing, "@pre", "@post", "@note", "@since"]


def _missing_from_block(block: str, args: list[str], ret: str) -> list[str]:
    """Every required tag the block does not carry."""
    missing = []
    if "@brief" not in block:
        missing.append("@brief")
    if "@details" not in block:
        missing.append("@details")
    for a in args:
        # match @param[...] name OR @param name (any direction)
        pat = re.compile(r"@param(?:\s*\[[^\]]*\])?\s+" + re.escape(a) + r"\b")
        if not pat.search(block):
            missing.append(f"@param[{a}]")
    if not is_returning_void(ret):
        if "@return" not in block and "@returns" not in block:
            missing.append("@return")
        if "@retval" not in block:
            missing.append("@retval")
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
    return missing


def _severity(missing: list[str]) -> str:
    """Rank a gap: a missing @brief or @param is worse than a missing @since."""
    if any(t == "@brief" or t.startswith("@param[") for t in missing):
        return "high"
    if any(t in ("@return", "@retval") or t.startswith(("@pre", "@post")) for t in missing):
        return "medium"
    return "low"


def audit_file(path: Path):
    """Return one row per function in ``path``: (file, line, name, missing, severity)."""
    try:
        raw = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    src_no_comments = strip_comments(raw)
    views = _SourceViews(path, raw, src_no_comments)
    # strip_comments preserves newlines, so line numbers agree between the
    # stripped and raw views and one offset conversion serves both.
    raw_lines = raw.splitlines(keepends=True)
    rel = str(path.relative_to(repo_root()))

    rows = []
    for m in FUNC_RE.finditer(src_no_comments):
        if not _is_declaration(m):
            continue
        line_no = src_no_comments.count("\n", 0, m.start()) + 1
        if line_no - 1 >= len(raw_lines):
            continue
        # Read the block from the ORIGINAL source, where comments still exist.
        offset_in_raw = sum(len(ln) for ln in raw_lines[: line_no - 1])
        block, has_block = find_preceding_doxy(raw, offset_in_raw)

        name, ret = m.group("name"), m.group("ret")
        if _is_waived(views, m, block, has_block=has_block):
            rows.append((rel, line_no, name, [], "ok"))
            continue

        args = parse_args(m.group("args"))
        missing = (
            _missing_from_block(block, args, ret)
            if has_block
            else _missing_without_block(args, ret)
        )
        if not missing:
            rows.append((rel, line_no, name, [], "ok"))
            continue
        rows.append((rel, line_no, name, missing, _severity(missing)))

    return rows
