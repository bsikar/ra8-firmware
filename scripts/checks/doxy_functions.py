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

# Contracts and same-file definitions may place a C23 attribute before the
# return type. Keep that broader spelling local to exact association: widening
# the historical repository audit parser would turn unrelated pre-existing
# header debt into a migration regression.
_HEADER_FUNC_RE = re.compile(
    r"^[ \t]*"
    r"(?P<ret>(?:(?:\[\[[^\]\n]*\]\]|static|inline|extern|const|volatile|register|signed|unsigned|"
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

# Contract association is deliberately narrower than C's full include model.
# Only an unconditional, directly included header beside the definition may
# own a private definition's contract. Inventing compiler search paths or
# following inactive/transitive includes can silently waive a real gap.
_PREPROCESSOR_RE = re.compile(r"^\s*#\s*(?P<name>[A-Za-z_][A-Za-z_0-9]*)(?P<body>.*)$")
_QUOTED_INCLUDE_VALUE_RE = re.compile(r'^\s*"(?P<path>[^"\n]+)"')

# Storage and repository visibility decorators are not part of the C function
# type. Linkage is compared separately, so removing ``static`` here does not
# allow a public declaration to satisfy a private definition.
_RETURN_DECORATOR_RE = re.compile(
    r"\b(?:static|inline|extern|register|RA8_INTERNAL|RA8_PRIV|RA8_WEAK)\b|"
    r"__attribute__\s*\(\([^)]*\)\)|\[\[[^\]\n]*\]\]"
)

# A suppression for the declaration itself is metadata, not a documentation
# attachment boundary. This spelling appears between the SysTick contract and
# its attributed definition.
_NOLINT_NEXT_RE = re.compile(
    r"(?:/\*[^\n]*\bNOLINTNEXTLINE\b[^\n]*\*/|//[^\n]*\bNOLINTNEXTLINE\b[^\n]*)\s*\Z"
)


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


def _param_name_info(param: str, position: int) -> tuple[str | None, bool]:
    """Return a parameter name and whether it is a generated stand-in."""
    if not param or param in {"void", "..."}:
        return None, False
    stripped = re.sub(r"\[[^\]]*\]", "", param).strip()
    # Function pointer: the name sits inside the (*name) group.
    fp = re.search(r"\(\s*\*\s*([A-Za-z_][A-Za-z_0-9]*)\s*\)", stripped)
    if fp:
        return fp.group(1), False
    toks = re.findall(r"[A-Za-z_][A-Za-z_0-9]*", stripped)
    if not toks:
        return None, False
    if len(toks) == 1 and toks[0] in _BARE_TYPE_KEYWORDS:
        return f"arg{position}", True
    return toks[-1], False


def _param_name(param: str, position: int) -> str | None:
    """Return the documented name of one parameter, or None when it has none."""
    name, _synthetic = _param_name_info(param, position)
    return name


def parse_args(args_text: str) -> list[str]:
    """Return the list of parameter names. ``(void)`` -> ``[]``."""
    names: list[str] = []
    for param in _split_params(args_text):
        name = _param_name(param, len(names))
        if name is not None:
            names.append(name)
    return names


def is_returning_void(ret: str) -> bool:
    """Whether a return-type string denotes plain ``void``.

    Answers the question behind the "documents a return value it cannot have"
    check, so it has to be tolerant of how the type was written: whitespace is
    collapsed and ``static`` / ``inline`` / ``extern`` / ``__attribute__((...))``
    are stripped before comparing.

    A pointer return is never void, which is checked BEFORE the name match --
    otherwise ``void *`` would read as void and every allocator in the tree
    would be reported for documenting its return.

    The trailing-word test also accepts a qualified spelling such as
    ``const void``.
    """
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


@dataclass(frozen=True)
class _FunctionSignature:
    """Function identity needed to associate a definition with a contract."""

    name: str
    return_type: str
    parameter_types: tuple[str, ...]
    is_static: bool


def _normalise_parameter_type(param: str, position: int) -> str:
    """Canonicalise one parameter type without depending on its local name."""
    text = re.sub(r"__attribute__\s*\(\([^)]*\)\)", "", param).strip()
    if text == "...":
        return text

    name, synthetic = _param_name_info(text, position)
    if name is not None and not synthetic:
        # A function-pointer name is nested in ``(*name)``. The ordinary
        # substitution below also works, but spelling this form explicitly
        # keeps the pointer declarator intact when whitespace differs.
        text = re.sub(
            rf"\(\s*\*\s*{re.escape(name)}\s*\)",
            "(*)",
            text,
            count=1,
        )
        text = re.sub(rf"\b{re.escape(name)}\b", "", text, count=1)

    # An array parameter is adjusted to a pointer by C. Treat ``T a[]`` and
    # ``T *a`` as the same signature while retaining every non-array token.
    text = re.sub(r"\[[^\]]*\]", "*", text)
    return re.sub(r"\s+", "", text)


def _function_signature(m: re.Match) -> _FunctionSignature:
    """Return a name-, type-, and linkage-aware signature for ``m``."""
    ret = m.group("ret")
    return _FunctionSignature(
        name=m.group("name"),
        return_type=re.sub(r"\s+", "", _RETURN_DECORATOR_RE.sub("", ret)),
        parameter_types=tuple(
            _normalise_parameter_type(param, position)
            for position, param in enumerate(_split_params(m.group("args")))
        ),
        is_static=bool(re.search(r"\bstatic\b", ret)),
    )


def _has_matching_peer(
    src_no_comments: str,
    m: re.Match,
    *,
    term: str,
    after: bool,
) -> bool:
    """Whether ``m`` has an exact declaration/definition peer in this file."""
    signature = _function_signature(m)
    for peer in _HEADER_FUNC_RE.finditer(src_no_comments):
        if not _is_declaration(peer) or peer.group("term") != term:
            continue
        if after and peer.start() <= m.start():
            continue
        if not after and peer.start() >= m.start():
            continue
        if _function_signature(peer) == signature:
            return True
    return False


def _has_bare_matching_prototype(raw: str, src_no_comments: str, m: re.Match) -> bool:
    """Whether ``m`` has an exact earlier prototype without its own block."""
    signature = _function_signature(m)
    for prototype in _HEADER_FUNC_RE.finditer(src_no_comments):
        if prototype.start() >= m.start() or prototype.group("term") != ";":
            continue
        if not _is_declaration(prototype) or _function_signature(prototype) != signature:
            continue
        _block, has_block = _preceding_block(raw, src_no_comments, prototype)
        if not has_block:
            return True
    return False


def _audit_matches(raw: str, src_no_comments: str) -> list[re.Match]:
    """Return historical matches plus attributed definitions with a local prototype."""
    matches = list(FUNC_RE.finditer(src_no_comments))
    starts = {match.start() for match in matches}
    for candidate in _HEADER_FUNC_RE.finditer(src_no_comments):
        if candidate.start() in starts or candidate.group("term") != "{":
            continue
        if not candidate.group(0).lstrip().startswith("[["):
            continue
        if _has_bare_matching_prototype(raw, src_no_comments, candidate):
            matches.append(candidate)
    return sorted(matches, key=lambda match: match.start())


def _matching_definition_has_contract(views: _SourceViews, m: re.Match) -> bool:
    """Whether any exact same-file definition owns the complete contract."""
    signature = _function_signature(m)
    for definition in _HEADER_FUNC_RE.finditer(views.stripped):
        if not _is_declaration(definition) or definition.group("term") != "{":
            continue
        if _function_signature(definition) != signature:
            continue
        block, has_block = _preceding_block(views.raw, views.stripped, definition)
        if not has_block:
            continue
        args = parse_args(definition.group("args"))
        if not _missing_from_block(block, args, definition.group("ret")) or _COPY_RE.search(block):
            return True
    return False


def _resolve_adjacent_include(including: Path, include: str) -> Path | None:
    """Resolve an include only when it names a header beside ``including``."""
    root = repo_root().resolve()
    source_dir = including.parent.resolve()
    resolved = (source_dir / include).resolve()
    try:
        resolved.relative_to(root)
    except ValueError:
        return None
    if resolved.parent != source_dir or not resolved.is_file() or resolved.suffix != ".h":
        return None
    return resolved


def _included_project_headers(path: Path, raw: str) -> list[Path]:
    """Return direct adjacent headers included outside preprocessor branches."""
    headers: list[Path] = []
    seen: set[Path] = set()
    conditional_depth = 0
    for line in strip_comments(raw).splitlines():
        directive = _PREPROCESSOR_RE.match(line)
        if directive is None:
            continue
        name = directive.group("name")
        if name in {"if", "ifdef", "ifndef"}:
            conditional_depth += 1
            continue
        if name == "endif":
            conditional_depth = max(0, conditional_depth - 1)
            continue
        if name != "include" or conditional_depth != 0:
            continue
        include = _QUOTED_INCLUDE_VALUE_RE.match(directive.group("body"))
        if include is None:
            continue
        header = _resolve_adjacent_include(path, include.group("path"))
        if header is not None and header not in seen:
            seen.add(header)
            headers.append(header)
    return headers


def _preceding_block(raw: str, stripped: str, m: re.Match) -> tuple[str, bool]:
    """Find the block attached to ``m`` across offset-changing comment stripping."""
    line_no = stripped.count("\n", 0, m.start()) + 1
    raw_lines = raw.splitlines(keepends=True)
    if line_no - 1 >= len(raw_lines):
        return "", False
    offset = sum(len(line) for line in raw_lines[: line_no - 1])
    block, has_block = find_preceding_doxy(raw, offset)
    if has_block:
        return block, True
    suppression = _NOLINT_NEXT_RE.search(raw[:offset])
    if suppression is None:
        return "", False
    return find_preceding_doxy(raw, suppression.start())


def _documented_header_signatures(path: Path, raw: str) -> set[_FunctionSignature]:
    """Collect complete contracts in direct, unconditional adjacent headers."""
    contracts: set[_FunctionSignature] = set()
    for header in _included_project_headers(path, raw):
        try:
            header_raw = header.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        header_stripped = strip_comments(header_raw)
        for declaration in _HEADER_FUNC_RE.finditer(header_stripped):
            if not _is_declaration(declaration) or declaration.group("term") != ";":
                continue
            block, has_block = _preceding_block(header_raw, header_stripped, declaration)
            if not has_block:
                continue
            args = parse_args(declaration.group("args"))
            if not _missing_from_block(block, args, declaration.group("ret")) or _COPY_RE.search(
                block
            ):
                contracts.add(_function_signature(declaration))
    return contracts


def _is_waived(
    views: _SourceViews,
    m: re.Match,
    block: str,
    documented_headers: set[_FunctionSignature],
    *,
    has_block: bool,
) -> bool:
    """True when this declaration legitimately needs no block of its own.

    Three waivers, each closing a contradiction rather than lowering the bar:

    * A non-static definition in a ``.c``. CLAUDE.md puts its authoritative
      block on the header declaration, which the ordinary header audit checks
      independently. Repeating the block on its definition would rot.
    * A static definition whose exact declaration and complete contract are in
      a directly and unconditionally included adjacent header. This supports
      explicit private contract headers without inventing compiler search
      paths or following inactive/transitive includes.
    * A file-local forward prototype with an exact definition below. The
      prototype is an ordering device, not a second contract. The definition
      remains audited, including when it has public linkage, so a bare pair is
      still reported once at the definition.
    * A block that defers to the header, or substitutes one with ``@copydoc``.
    """
    ret = m.group("ret")
    if views.path.suffix == ".c" and m.group("term") == "{":
        if _function_signature(m) in documented_headers:
            return True
        has_bare_local_prototype = _has_bare_matching_prototype(
            views.raw,
            views.stripped,
            m,
        )
        if has_bare_local_prototype and _matching_definition_has_contract(views, m):
            return True
        if not re.search(r"\bstatic\b", ret) and not has_bare_local_prototype:
            return True
    if (
        m.group("term") == ";"
        and not has_block
        and _has_matching_peer(views.stripped, m, term="{", after=True)
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


def audit_file(path: Path) -> list[tuple[str, int, str, str, str]]:
    """Return one row per function in ``path``: (file, line, name, missing, severity)."""
    try:
        raw = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    src_no_comments = strip_comments(raw)
    views = _SourceViews(path, raw, src_no_comments)
    documented_headers = _documented_header_signatures(path, raw) if path.suffix == ".c" else set()
    # strip_comments preserves newlines, so line numbers agree between the
    # stripped and raw views and one offset conversion serves both.
    raw_lines = raw.splitlines(keepends=True)
    rel = str(path.relative_to(repo_root()))

    rows = []
    for m in _audit_matches(raw, src_no_comments):
        if not _is_declaration(m):
            continue
        line_no = src_no_comments.count("\n", 0, m.start()) + 1
        if line_no - 1 >= len(raw_lines):
            continue
        # Read the block from the ORIGINAL source, where comments still exist.
        block, has_block = _preceding_block(raw, src_no_comments, m)

        name, ret = m.group("name"), m.group("ret")
        if _is_waived(
            views,
            m,
            block,
            documented_headers,
            has_block=has_block,
        ):
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
