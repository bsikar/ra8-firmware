#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: a Doxygen block must describe the thing it is actually attached to.

The presence gates (``doxy_audit.py --check`` for functions,
``doxy_audit.py --members --check`` for members/enums/macros) only ask *is a
block there*.  They cannot see a block that is there but describes something
else, and a misattached block actively **satisfies** them: paste one block
twice and measured "coverage" goes up while one symbol silently loses its
documentation and another gains a duplicate.  Every defect this gate finds was
therefore invisible to -- and in some cases rewarded by -- the existing gates.

Real defects that motivated this (both found by eye, in ``tools/ra8_viewer``):

* ``main()``'s block sat immediately above ``viewer_log_sink()``'s block, so
  ``main()`` was undocumented and the sink was documented twice.
* ``viewer_compute_tiles()`` carried its block on a forward declaration while
  the definition below it sat bare.

Why a sibling script rather than a new ``doxy_audit.py`` mode:

* **Different question, different parser.**  ``doxy_audit.py`` is regex-driven
  end to end; its ``FUNC_RE`` + ``strip_comments`` design answers "is a tag
  present".  Attachment needs real parameter names, real return types and real
  declaration-vs-definition identity -- an AST question.  Bolting a libclang
  mode onto a regex tool means two parsers, two scope constants and two notions
  of "a function" in one file, which is precisely the confusion that let
  ``doxy_audit.py``'s own scope hole (no ``tools/``, ``tests/``, ``examples/``)
  survive unnoticed.
* **Different dependency.**  This gate hard-requires libclang and must fail
  loudly without it (see ``_require_libclang``).  ``doxy_audit.py`` has no
  third-party dependency and must keep working in environments that lack one.
* **Single Responsibility** (CLAUDE.md, SOLID for C): presence and correctness
  are separate concerns with separate failure modes and separate fix
  procedures.

Scope: every first-party ``.c`` / ``.h`` under ``libs/``, ``src/``, ``port/``,
``examples/``, ``tools/`` and ``tests/``.  Vendored SOUP (``libs/third_party``)
and generated data (``libs/fonts``) are excluded, matching CLAUDE.md.

Run::

    check_doc_attachment.py --check     # CI gate (exit 1 on any finding)
    check_doc_attachment.py             # audit listing, exit 0
    check_doc_attachment.py --selftest  # synthetic both-direction fixtures
    check_doc_attachment.py PATH ...    # restrict to the given files/dirs

Exit 0 when clean, 1 on findings in ``--check`` mode, 2 on a selftest failure
or a missing/unusable libclang.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

#: First-party roots.  Mirrors the CLAUDE.md "Scope" note: every first-party
#: file, not just the firmware.  ``tools/`` and ``tests/`` are in deliberately
#: -- leaving them out is how a whole host emulator went ungated before.
SCAN_ROOTS = ("libs", "src", "port", "examples", "tools", "tests")

#: A symbol must appear at least this many times (declaration + definition)
#: before a forward-declaration-vs-definition split can even exist.
MIN_REDECLARATIONS = 2

#: Path fragments that mark non-first-party or generated trees.
EXCLUDED_PARTS = frozenset(
    {
        "third_party",
        "fonts",
        "build",
        "build-cov",
        "build-bench",
        "build-scan",
        "build-mcdc",
        "build-sim",
        "_deps",
        "CMakeFiles",
    }
)

SOURCE_SUFFIXES = (".c", ".h")

# ---------------------------------------------------------------------------
# Finding codes.  Ordered by signal strength: a DOC001 is near-certain paste
# evidence, a DOC005 is strong, a DOC004 is the owner's literal complaint.
# ---------------------------------------------------------------------------
CODE_HELP = {
    "DOC001": "@param names a parameter the signature does not have",
    "DOC002": "partially documented signature -- some parameters have no @param",
    "DOC003": "@return/@retval on a function returning void",
    "DOC004": "two doc blocks in a row with no declaration between them",
    "DOC005": "block names a different symbol than the one it is attached to",
    "DOC006": "block sits on a forward declaration whose definition is bare",
    "DOC007": "banned pointer-only definition-site boilerplate",
}

# ---------------------------------------------------------------------------
# Doxygen tag scanning.  Doxygen accepts both `@tag` and `\tag`.
# ---------------------------------------------------------------------------
_T = r"[@\\]"

#: ``@param[in] name`` / ``@param[in,out] a,b`` / ``@param name``.
PARAM_RE = re.compile(
    _T + r"param\s*(?:\[[^\]]*\])?\s+"
    r"([A-Za-z_][A-Za-z_0-9]*(?:\s*,\s*[A-Za-z_][A-Za-z_0-9]*)*)"
)
RETURN_RE = re.compile(_T + r"returns?\b")
RETVAL_RE = re.compile(_T + r"retval\b")
COPY_RE = re.compile(_T + r"copy(?:doc|details|brief)\b")

#: Explicit "this block documents symbol X" tags.  These are unambiguous: if
#: the tag names X and the block is attached to Y, one of the two is wrong.
EXPLICIT_REF_RE = re.compile(
    _T + r"(fn|struct|enum|union|def|var|typedef|class)\s+([A-Za-z_][A-Za-z_0-9]*)"
)

#: The CLAUDE.md-sanctioned definition-site single-line form:
#:   /** @brief Implementation of `ra8_err_to_str()` -- linear-scan lookup. */
#: The backticked name is a machine-checkable claim about which function this
#: is; a pasted block carries the wrong one.
IMPL_OF_RE = re.compile(r"[Ii]mplementation of\s+`([A-Za-z_][A-Za-z_0-9]*)\s*\(\)`")

#: Blocks that legitimately stand alone (documentation structure, not a
#: symbol).  A block of this kind directly above another block is normal and
#: must never be reported as a duplicate.
STANDALONE_TAG_RE = re.compile(
    _T + r"(file|dir|mainpage|page|subpage|section|subsection|defgroup|addtogroup"
    r"|ingroup|weakgroup|name|cond|endcond|example|internal|endinternal"
    r"|copyright|brief\s*$)"
)

#: Doxygen grouping markers -- ``/** @{ */`` and ``/** @} */`` sit between two
#: real blocks all the time.
GROUP_MARKER_RE = re.compile(r"[@\\][{}]")

#: An explicit statement that a function yields no value -- either because it
#: returns void ("Nothing.", "None.") or because it never returns at all
#: ("This function never returns.", on the ``[[noreturn]] void`` handlers and
#: park loops).  Doxygen prefers these be omitted, but they are a deliberate,
#: self-consistent house style here and they state the truth about the
#: signature, so they are not a contradiction.  ``@retval`` on a void function
#: is different: it enumerates return *values* that cannot exist.
RETURN_NOTHING_RE = re.compile(
    _T + r"returns?\s+"
    r"(?:nothing|none|void|n/?a|no value"
    r"|(?:this function |the function )?(?:never returns|does not return|no return))"
    r"\b[.\s]*",
    re.IGNORECASE,
)

#: CLAUDE.md "BANNED (rejected in review)" definition-site boilerplate: a
#: comment whose only content is a pointer back at the header.
BANNED_BOILERPLATE_RE = re.compile(
    r"(?:see (?:the )?(?:public )?header for "
    r"(?:the |full )?(?:documented )?(?:contract|description)"
    r"|see header for full contract)",
    re.IGNORECASE,
)


@dataclass(frozen=True)
class Finding:
    """One gate finding."""

    path: str
    line: int
    code: str
    symbol: str
    detail: str

    def render(self) -> str:
        return f"  {self.path}:{self.line}  {self.code}  {self.symbol}  --  {self.detail}"


@dataclass
class DocBlock:
    """A lexically-extracted ``/** ... */`` or ``/*! ... */`` block."""

    start_line: int
    end_line: int
    text: str
    #: True for blocks that document the file / a group rather than a symbol.
    standalone: bool = False
    #: True for the ``/**<`` "documents the *preceding* member" form.  These
    #: are trailing comments on struct fields and enum values; a run of them on
    #: consecutive lines is the normal shape and must never read as duplicates.
    trailing: bool = False


@dataclass
class DocTags:
    """The claims a doc block makes, extracted once."""

    params: list[str] = field(default_factory=list)
    has_return: bool = False
    has_retval: bool = False
    has_copy: bool = False
    explicit_refs: list[tuple[str, str]] = field(default_factory=list)
    impl_of: str | None = None


# ---------------------------------------------------------------------------
# libclang
# ---------------------------------------------------------------------------
def _require_libclang():
    """Import libclang or exit(2).

    Deliberately fatal.  ``check_annotations.py`` used to ``exit(0)`` here,
    which made a container missing the binding read as a clean strict gate --
    strictly worse than not running the gate at all, because it reports
    success.  A gate that cannot run has not passed.
    """
    try:
        from clang import cindex  # noqa: PLC0415  # probe-then-import is the point
    except ImportError:
        sys.stderr.write(
            "check_doc_attachment.py: FATAL -- the 'libclang' Python binding is missing,\n"
            "  so the documentation-attachment gate cannot run. This is an error, not a\n"
            "  skip: a gate that cannot run has not passed.\n"
            "  install: python3 -m pip install --user --break-system-packages libclang\n"
        )
        sys.exit(2)
    return cindex


def _include_args(cindex) -> list[str]:  # noqa: ARG001  # kept for signature parity
    """``-I`` flags for every first-party header root."""
    roots: list[Path] = []
    for pattern in (
        "libs/*/inc",
        "libs/*/src",
        "src/inc",
        "src/secure_app",
        "tests/include",
        "tests",
        "port/*/inc",
        "tools/*/inc",
    ):
        roots.extend(sorted(REPO_ROOT.glob(pattern)))
    return [f"-I{d}" for d in roots if d.is_dir() and "third_party" not in d.parts]


# ---------------------------------------------------------------------------
# Lexical pass
# ---------------------------------------------------------------------------
def _blank_comments_and_literals(text: str) -> str:
    """Blank every comment and string/char literal, preserving line structure.

    Length and newline positions are preserved so the result can be indexed by
    the same line numbers as the original.  Blanking (rather than deleting) is
    what lets the DOC004 "is there code between these two blocks" test look at
    code only -- an earlier version advanced past comments without clearing
    them, so a member's own trailing ``/**< ... */`` counted as intervening
    code and the check silently never fired on real member runs.
    """
    out = list(text)

    def blank(lo: int, hi: int) -> None:
        for k in range(lo, min(hi, len(text))):
            if text[k] != "\n":
                out[k] = " "

    i, n = 0, len(text)
    while i < n:
        ch = text[i]
        if ch in {'"', "'"}:
            quote = ch
            start = i
            i += 1
            while i < n:
                if text[i] == "\\" and i + 1 < n:
                    i += 2
                    continue
                if text[i] == quote:
                    i += 1
                    break
                i += 1
            blank(start, i)
            continue
        if ch == "/" and i + 1 < n and text[i + 1] == "/":
            start = i
            while i < n and text[i] != "\n":
                i += 1
            blank(start, i)
            continue
        if ch == "/" and i + 1 < n and text[i + 1] == "*":
            start = i
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                i += 1
            i += 2
            blank(start, i)
            continue
        i += 1
    return "".join(out)


def _skip_literal(text: str, i: int, line: int) -> tuple[int, int]:
    """Advance past the string/char literal starting at ``i``."""
    quote = text[i]
    n = len(text)
    i += 1
    while i < n:
        if text[i] == "\\" and i + 1 < n:
            i += 2
            continue
        if text[i] == quote:
            i += 1
            break
        if text[i] == "\n":
            line += 1
        i += 1
    return i, line


def _scan_block_comment(text: str, i: int, line: int) -> tuple[int, int, bool]:
    """Advance past the ``/* ... */`` at ``i``; report whether it is a doc block."""
    n = len(text)
    is_doc = i + 2 < n and text[i + 2] in {"*", "!"}
    # `/**/` and `/***/` are terminators, not documentation.
    if is_doc and i + 3 < n and text[i + 2] == "*" and text[i + 3] == "/":
        is_doc = False
    j = i + 2
    while j + 1 < n and not (text[j] == "*" and text[j + 1] == "/"):
        if text[j] == "\n":
            line += 1
        j += 1
    return j + 2, line, is_doc


def extract_doc_blocks(text: str) -> list[DocBlock]:
    """Return every ``/**`` / ``/*!`` block in ``text``, in source order.

    Blocks inside string literals are skipped.  Single-line ``///`` and ``//!``
    comments are captured too so the duplicate check sees them.
    """
    blocks: list[DocBlock] = []
    i, n = 0, len(text)
    line = 1
    while i < n:
        ch = text[i]
        if ch == "\n":
            line += 1
            i += 1
            continue
        if ch in {'"', "'"}:
            i, line = _skip_literal(text, i, line)
            continue
        if ch == "/" and i + 1 < n and text[i + 1] == "*":
            start_line = line
            end, line, is_doc = _scan_block_comment(text, i, line)
            if is_doc:
                blocks.append(DocBlock(start_line, line, text[i:end]))
            i = end
            continue
        if ch == "/" and i + 1 < n and text[i + 1] == "/":
            is_doc = i + 2 < n and text[i + 2] in {"/", "!"}
            chunk_start = i
            while i < n and text[i] != "\n":
                i += 1
            if is_doc:
                blocks.append(DocBlock(line, line, text[chunk_start:i]))
            continue
        i += 1
    for b in blocks:
        b.standalone = bool(STANDALONE_TAG_RE.search(b.text) or GROUP_MARKER_RE.search(b.text))
        b.trailing = bool(re.match(r"/(?:\*[*!]|//|/!)<", b.text))
    return blocks


def parse_tags(block_text: str) -> DocTags:
    """Extract the claims a block makes.

    ``@code``/``@endcode`` bodies are removed first: a usage example may
    legitimately mention another function's parameters, and reading tags out of
    one produces false positives.
    """
    body = re.sub(r"[@\\]code\b.*?[@\\]endcode\b", " ", block_text, flags=re.DOTALL)
    params: list[str] = []
    for m in PARAM_RE.finditer(body):
        params.extend(p.strip() for p in m.group(1).split(","))
    # A bare "@return Nothing." on a void function states the truth; only a
    # @return that promises an actual value contradicts a void signature.
    return DocTags(
        params=params,
        has_return=bool(RETURN_RE.search(RETURN_NOTHING_RE.sub(" ", body))),
        has_retval=bool(RETVAL_RE.search(body)),
        has_copy=bool(COPY_RE.search(body)),
        explicit_refs=EXPLICIT_REF_RE.findall(body),
        impl_of=(m.group(1) if (m := IMPL_OF_RE.search(body)) else None),
    )


def check_consecutive_blocks(path: str, text: str) -> list[Finding]:
    """DOC004 -- two doc blocks in a row with nothing declared between them.

    This is the owner's literal complaint and the shape that makes a presence
    gate *reward* the defect.  A ``@file`` / ``@defgroup`` / ``@{`` block
    followed by a real block is normal and is exempted via ``standalone``.
    """
    findings: list[Finding] = []
    blocks = extract_doc_blocks(text)
    lines = _blank_comments_and_literals(text).splitlines()
    raw_lines = text.splitlines()
    for prev, cur in zip(blocks, blocks[1:]):
        if prev.standalone or prev.trailing or cur.trailing:
            continue
        # A block carrying an explicit @def / @fn / @struct / ... tag is
        # resolved by Doxygen *by name*, not by position, so it does not need a
        # following declaration at all.  Config headers rely on this to
        # document deliberately-disabled options:
        #     /** \def MBEDTLS_AES_ROM_TABLES ... */
        #     //#define MBEDTLS_AES_ROM_TABLES
        # The commented-out define is not code, so a purely positional test
        # calls that a duplicate.  Accept the block when the very next non-blank
        # source line mentions the symbol it names -- which still leaves a block
        # whose symbol is declared further down, past another symbol, reported.
        refs = [ref for _, ref in EXPLICIT_REF_RE.findall(prev.text)]
        if refs:
            following = next(
                (ln for ln in raw_lines[prev.end_line : cur.start_line - 1] if ln.strip()),
                "",
            ) or next((ln for ln in raw_lines[prev.end_line :] if ln.strip()), "")
            if any(re.search(r"\b" + re.escape(r) + r"\b", following) for r in refs):
                continue
        # Anything but whitespace between the two blocks means the first one is
        # attached to something -- not a duplicate.
        between = lines[prev.end_line : cur.start_line - 1]
        if any(seg.strip() for seg in between):
            continue
        findings.append(
            Finding(
                path,
                prev.start_line,
                "DOC004",
                "(block)",
                f"doc block is followed by another doc block at line {cur.start_line} "
                "with no declaration between them; the first documents nothing",
            )
        )
    return findings


def check_banned_boilerplate(path: str, text: str) -> list[Finding]:
    """DOC007 -- CLAUDE.md's banned pointer-only definition-site comment.

    Only fires on blocks whose *entire* informational content is the pointer.
    A block that says "see the header for the full contract" and then adds a
    real implementation note is allowed by CLAUDE.md, so the presence of any
    other Doxygen tag or a ``--`` note clause clears it.
    """
    findings: list[Finding] = []
    for block in extract_doc_blocks(text):
        if not BANNED_BOILERPLATE_RE.search(block.text):
            continue
        body = re.sub(r"^\s*/\*+<?|\*+/\s*$", " ", block.text)
        body = re.sub(r"^\s*\*", " ", body, flags=re.MULTILINE)
        tags = set(re.findall(_T + r"([a-z]+)", body))
        # @brief alone is still boilerplate; any other tag means real content.
        if tags - {"brief"}:
            continue
        # Strip everything the idiom is *allowed* to contain -- the @brief tag,
        # the "Implementation of <symbol>()" lead-in (backticked or not), the
        # pointer phrase itself, and punctuation.  Whatever survives is the
        # real implementation note CLAUDE.md requires.  If nothing survives,
        # the comment says only "see the header" and is banned.
        residue = BANNED_BOILERPLATE_RE.sub(" ", body)
        residue = re.sub(_T + r"brief", " ", residue)
        residue = re.sub(
            r"[Ii]mplementation of\s+`?[A-Za-z_][A-Za-z_0-9]*`?(?:\s*\(\))?`?", " ", residue
        )
        residue = re.sub(r"[`(){}.,;:*/\s-]", "", residue)
        if residue:
            continue
        findings.append(
            Finding(
                path,
                block.start_line,
                "DOC007",
                "(block)",
                "pointer-only definition-site boilerplate (CLAUDE.md bans it): the comment "
                "carries no information the signature does not. Delete it, or replace it "
                "with the single-line form carrying a real implementation note",
            )
        )
    return findings


# ---------------------------------------------------------------------------
# AST pass
# ---------------------------------------------------------------------------
def _param_names(cursor, cindex) -> list[str]:
    """Named parameters of a function cursor, in order."""
    return [
        a.spelling
        for a in cursor.get_arguments()
        if a.kind == cindex.CursorKind.PARM_DECL and a.spelling
    ]


def _is_macro_expanded(cursor) -> bool:
    """True when the declaration came out of a macro expansion.

    X-macro tables and declaration-generating macros produce cursors whose
    spelling never appears in the source text, so parameter and name checks
    against them are meaningless.
    """
    try:
        return cursor.location.is_from_main_file() is False and cursor.extent.start.file is None
    except (AttributeError, ValueError):
        return False


def typedef_names_for_anonymous(tu, cindex, own_file: str) -> dict[tuple[int, int], str]:
    """Map each anonymous record/enum's location to the typedef that names it.

    ``typedef struct { ... } sim_args_t;`` -- the C23 shape this codebase uses
    almost everywhere -- produces a STRUCT_DECL whose spelling is
    ``struct (unnamed at ...)``.  Comparing ``@struct sim_args_t`` against that
    spelling reports every correctly-documented struct in the tree as a name
    mismatch.  The typedef is the symbol's real name, so resolve it.
    """
    out: dict[tuple[int, int], str] = {}
    for cursor in tu.cursor.walk_preorder():
        if cursor.kind != cindex.CursorKind.TYPEDEF_DECL:
            continue
        loc = cursor.location.file
        if loc is None or os.path.realpath(loc.name) != own_file:
            continue
        try:
            decl = cursor.underlying_typedef_type.get_declaration()
        except (AttributeError, ValueError):
            continue
        if decl is None or decl.location.file is None:
            continue
        out[(decl.location.line, decl.location.column)] = cursor.spelling
    return out


def _display_name(cursor, anon_names: dict[tuple[int, int], str]) -> str:
    """The name a doc block would reasonably use for ``cursor``."""
    spelling = cursor.spelling or ""
    if spelling and "(unnamed" not in spelling and "(anonymous" not in spelling:
        return spelling
    return anon_names.get((cursor.location.line, cursor.location.column), "")


def check_symbol(
    cursor,
    cindex,
    path: str,
    anon_names: dict[tuple[int, int], str],
    attached: dict[int, DocBlock],
) -> list[Finding]:
    """Run every attachment check for one documented declaration.

    Attachment is resolved **positionally** (``attached``), never via
    ``cursor.raw_comment``.  libclang propagates a comment to every
    redeclaration of a symbol, so a block on a forward declaration also reports
    as the definition's own comment -- which double-reported every finding and
    would have made the eventual fix look incomplete.  The block a reader sees
    above a declaration is the one this gate judges.
    """
    findings: list[Finding] = []
    block = _decl_block(cursor, attached)
    if block is None:
        return findings
    tags = parse_tags(block.text)
    name = _display_name(cursor, anon_names)
    line = cursor.location.line
    # An anonymous record with no naming typedef has no name a block could be
    # checked against; the tag checks below would compare against "".
    if not name:
        return findings

    # `@copydoc` substitutes another symbol's block wholesale at render time,
    # so its tags describe that symbol by design. Nothing to cross-check.
    if tags.has_copy:
        return findings

    # -- DOC005: an explicit @fn/@struct/@enum/... naming a different symbol.
    for kind, ref in tags.explicit_refs:
        if kind in {"ingroup", "name"}:
            continue
        if ref != name:
            findings.append(
                Finding(
                    path,
                    line,
                    "DOC005",
                    name,
                    f"block says '@{kind} {ref}' but is attached to '{name}'",
                )
            )
            break

    # -- DOC005: the sanctioned definition-site form naming a different symbol.
    if tags.impl_of and tags.impl_of != name:
        findings.append(
            Finding(
                path,
                line,
                "DOC005",
                name,
                f"block says 'Implementation of `{tags.impl_of}()`' but is attached to '{name}'",
            )
        )

    if cursor.kind != cindex.CursorKind.FUNCTION_DECL:
        return findings

    actual = _param_names(cursor, cindex)

    # -- DOC001: a documented parameter the signature does not have.
    documented = [p for p in tags.params if p]
    unknown = [p for p in documented if p not in actual]
    if unknown and actual is not None:
        findings.append(
            Finding(
                path,
                line,
                "DOC001",
                name,
                f"@param {', '.join(sorted(set(unknown)))} -- no such parameter "
                f"(signature: {', '.join(actual) or 'void'})",
            )
        )

    # -- DOC002: a partially documented signature.  Only fires when the block
    # already documents at least one parameter: a block with no @param at all
    # is a *presence* gap and belongs to doxy_audit.py, not here.  A block that
    # documents 1 of 8 is drift or paste residue -- an attachment defect.
    if documented:
        missing = [p for p in actual if p not in documented]
        if missing:
            findings.append(
                Finding(
                    path,
                    line,
                    "DOC002",
                    name,
                    f"documents {len(documented)} of {len(actual)} parameters; "
                    f"no @param for: {', '.join(missing)}",
                )
            )

    # -- DOC003: @return / @retval on a void function.
    if cursor.result_type.kind == cindex.TypeKind.VOID and (tags.has_return or tags.has_retval):
        which = "@retval" if tags.has_retval else "@return"
        findings.append(
            Finding(path, line, "DOC003", name, f"{which} documented but the function returns void")
        )

    return findings


def blocks_by_attach_line(text: str) -> dict[int, DocBlock]:
    """Map each doc block to the source line of the construct it precedes.

    Needed because ``cursor.raw_comment`` is **not** positional: libclang
    propagates a comment across every redeclaration of a symbol, so the bare
    definition of a function whose forward declaration carries a block reports
    that same block as its own.  Asking libclang alone therefore made DOC006
    structurally unable to fire.  This resolves attachment lexically instead,
    which is what "attached to" actually means in the source.
    """
    code = _blank_comments_and_literals(text).splitlines()
    out: dict[int, DocBlock] = {}
    for block in extract_doc_blocks(text):
        if block.trailing:
            continue
        for idx in range(block.end_line, len(code)):
            if code[idx].strip():
                out[idx + 1] = block
                break
    return out


def _decl_block(cursor, attached: dict[int, DocBlock]) -> DocBlock | None:
    """The doc block lexically preceding ``cursor``, or None.

    A declaration can start above ``cursor.location.line`` -- annotation
    macros, a return type on its own line, ``static`` on the previous line --
    so every line from the extent start through the name line is a candidate
    landing spot.
    """
    try:
        first = cursor.extent.start.line
    except (AttributeError, ValueError):
        first = cursor.location.line
    for ln in range(min(first, cursor.location.line), cursor.location.line + 1):
        if ln in attached:
            return attached[ln]
    return None


def _decls_between(all_decls: list, first, second) -> bool:
    """True when some *other* function is declared between ``first`` and ``second``."""
    lo, hi = first.location.line, second.location.line
    return any(lo < c.location.line < hi and c.spelling != first.spelling for c in all_decls)


def check_forward_decl_blocks(tu, cindex, path: str, own_file: str, text: str) -> list[Finding]:
    """DOC006 -- a block on a forward declaration whose definition is bare.

    Scoped to a single file on purpose.  A block on a *header* declaration with
    a bare definition in the .c is exactly what CLAUDE.md prescribes; the defect
    is the in-file forward declaration that hoards the block and leaves the
    definition below it undocumented.
    """
    attached = blocks_by_attach_line(text)
    decls: dict[str, list] = {}
    for cursor in tu.cursor.walk_preorder():
        if cursor.kind != cindex.CursorKind.FUNCTION_DECL:
            continue
        loc = cursor.location.file
        if loc is None or os.path.realpath(loc.name) != own_file:
            continue
        decls.setdefault(cursor.spelling, []).append(cursor)

    all_decls = sorted(
        (c for group in decls.values() for c in group), key=lambda c: c.location.line
    )
    findings: list[Finding] = []
    for name, cursors in decls.items():
        if len(cursors) < MIN_REDECLARATIONS:
            continue
        definition = next((c for c in cursors if c.is_definition()), None)
        if definition is None:
            continue
        # Positional, not cursor.raw_comment -- see blocks_by_attach_line().
        if _decl_block(definition, attached) is not None:
            continue
        documented_decl = next(
            (c for c in cursors if not c.is_definition() and _decl_block(c, attached) is not None),
            None,
        )
        if documented_decl is None:
            continue
        # The `-Wmissing-prototypes` idiom puts a local prototype directly above
        # its own definition:
        #     /** ... */
        #     void NMI_Handler(void);
        #     void NMI_Handler(void) { ... }
        # The block sits immediately above both, so no reader is misled and
        # CLAUDE.md's "the authoritative block lives on the declaration" is
        # satisfied. The defect is the block that got *separated* from its
        # definition by other code -- ra8_rsip.c documented internal_sw_sha256
        # 612 lines above the body, with a dozen other functions in between.
        # Fire only when something else is declared between the two; no
        # line-count threshold is involved.
        if not _decls_between(all_decls, documented_decl, definition):
            continue
        findings.append(
            Finding(
                path,
                documented_decl.location.line,
                "DOC006",
                name,
                f"doc block sits on the forward declaration while the definition at line "
                f"{definition.location.line} is bare; move the block to the definition",
            )
        )
    return findings


def check_file(path: Path, cindex, args: list[str]) -> list[Finding]:
    """Run the lexical + AST checks over one file."""
    # Ad-hoc runs may name a file outside the repo (bisecting a historical
    # revision into a scratch dir, for one); fall back to the absolute path
    # rather than raising.
    try:
        rel = str(path.relative_to(REPO_ROOT))
    except ValueError:
        rel = str(path)
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []

    findings = check_consecutive_blocks(rel, text) + check_banned_boilerplate(rel, text)

    index = cindex.Index.create()
    try:
        tu = index.parse(str(path), args=args)
    except cindex.TranslationUnitLoadError:
        return findings
    if tu is None:
        return findings

    own = os.path.realpath(str(path))
    interesting = {
        cindex.CursorKind.FUNCTION_DECL,
        cindex.CursorKind.STRUCT_DECL,
        cindex.CursorKind.UNION_DECL,
        cindex.CursorKind.ENUM_DECL,
        cindex.CursorKind.TYPEDEF_DECL,
        cindex.CursorKind.FIELD_DECL,
        cindex.CursorKind.ENUM_CONSTANT_DECL,
        cindex.CursorKind.VAR_DECL,
    }
    anon_names = typedef_names_for_anonymous(tu, cindex, own)
    attached = blocks_by_attach_line(text)
    seen: set[tuple[int, str]] = set()
    for cursor in tu.cursor.walk_preorder():
        if cursor.kind not in interesting:
            continue
        loc = cursor.location.file
        if loc is None or os.path.realpath(loc.name) != own:
            continue
        key = (cursor.location.line, cursor.spelling or "")
        if key in seen:
            continue
        seen.add(key)
        if _is_macro_expanded(cursor):
            continue
        findings.extend(check_symbol(cursor, cindex, rel, anon_names, attached))

    findings.extend(check_forward_decl_blocks(tu, cindex, rel, own, text))
    return findings


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------
def iter_sources(targets: list[Path]) -> list[Path]:
    """Every in-scope source file under ``targets``."""
    out: list[Path] = []
    for target in targets:
        if target.is_file():
            if target.suffix in SOURCE_SUFFIXES:
                out.append(target)
            continue
        for dirpath, dirnames, filenames in os.walk(target):
            dirnames[:] = [d for d in dirnames if d not in EXCLUDED_PARTS]
            for fn in filenames:
                if not fn.endswith(SOURCE_SUFFIXES):
                    continue
                p = Path(dirpath) / fn
                if EXCLUDED_PARTS & set(p.relative_to(REPO_ROOT).parts):
                    continue
                out.append(p)
    return sorted(set(out))


def run(targets: list[Path], strict: bool) -> int:
    """Scan ``targets`` and report."""
    cindex = _require_libclang()
    args = ["-std=c23", "-x", "c", "-DRA8_HOST_BUILD=1", *_include_args(cindex)]

    files = iter_sources(targets)
    findings: list[Finding] = []
    for path in files:
        findings.extend(check_file(path, cindex, args))

    findings.sort(key=lambda f: (f.path, f.line, f.code))
    if not findings:
        print(f"check_doc_attachment: files={len(files)} findings=0 (PASS)")
        return 0

    by_code: dict[str, int] = {}
    for f in findings:
        by_code[f.code] = by_code.get(f.code, 0) + 1

    verdict = "FAIL" if strict else "audit"
    print(f"check_doc_attachment: files={len(files)} findings={len(findings)} ({verdict})")
    for code in sorted(by_code):
        print(f"  {code}  x{by_code[code]:<5} {CODE_HELP[code]}")
    print()
    for f in findings:
        print(f.render())
    return 1 if strict else 0


# ---------------------------------------------------------------------------
# Selftest
# ---------------------------------------------------------------------------
#: (name, source, expected finding codes).  Every defect class must be proven
#: to FIRE, and every tricky-but-legal form proven NOT to.  A gate that cries
#: wolf gets disabled, which is worse than no gate.
SELFTEST_CASES: list[tuple[str, str, set[str]]] = [
    # ---------------- must FIRE ----------------
    (
        "param name pasted from another function",
        """
/**
 * @brief Add two numbers.
 * @param[in] reader Open reader.
 * @param[in] tile   Tile index.
 * @return Sum.
 */
int add_values(int lhs, int rhs) { return lhs + rhs; }
""",
        {"DOC001", "DOC002"},
    ),
    (
        "param documented that does not exist",
        """
/**
 * @brief Add two numbers.
 * @param[in] lhs Left.
 * @param[in] rhs Right.
 * @param[in] carry Nonexistent.
 * @return Sum.
 */
int add_values(int lhs, int rhs) { return lhs + rhs; }
""",
        {"DOC001"},
    ),
    (
        "partially documented signature (1 of 3)",
        """
/**
 * @brief Download one chapter.
 * @param[in] url Source URL.
 * @return Status.
 */
int download_chapter(const char* url, const char* dest, int retries) { return 0; }
""",
        {"DOC002"},
    ),
    (
        "retval on a void function",
        """
/**
 * @brief Reset the widget.
 * @param[in] id Widget id.
 * @retval 0 Success.
 */
void widget_reset(int id) { (void)id; }
""",
        {"DOC003"},
    ),
    (
        "@return promising a value on a void function",
        """
/**
 * @brief Reset the widget.
 * @param[in] id Widget id.
 * @return The number of registers cleared.
 */
void widget_reset(int id) { (void)id; }
""",
        {"DOC003"},
    ),
    (
        "two doc blocks in a row (the ra8_viewer main/log_sink shape)",
        """
/**
 * @brief Program entry point.
 * @return 0 on success.
 */

/**
 * @brief Log sink.
 * @param[in] byte Byte to emit.
 */
static void log_sink(unsigned char byte) { (void)byte; }
""",
        {"DOC004"},
    ),
    (
        "identical block pasted twice",
        """
/**
 * @brief Compute the checksum.
 * @param[in] len Length.
 * @return Checksum.
 */
/**
 * @brief Compute the checksum.
 * @param[in] len Length.
 * @return Checksum.
 */
int checksum(int len) { return len; }
""",
        {"DOC004"},
    ),
    (
        "@fn naming a different function",
        """
/**
 * @fn viewer_open_comic
 * @brief Open an RTA1 atlas.
 * @param[in] r Reader.
 * @return Status.
 */
int viewer_open_rta1(int r) { return r; }
""",
        {"DOC005"},
    ),
    (
        "@struct naming a different struct",
        """
/**
 * @struct lcd_config_t
 * @brief Panel timing.
 */
struct panel_timing_t { int hsync; };
""",
        {"DOC005"},
    ),
    (
        "@enum naming a different enum",
        """
/**
 * @enum lcd_state_t
 * @brief Reader states.
 */
enum reader_state_t { k_idle = 0 };
""",
        {"DOC005"},
    ),
    (
        "definition-site 'Implementation of `X()`' naming a different function",
        """
/** @brief Implementation of `ra8_err_to_str()` -- linear-scan lookup. */
int ra8_err_to_code(int c) { return c; }
""",
        {"DOC005"},
    ),
    (
        "block on a forward declaration separated from its definition by other code",
        """
/**
 * @brief Probe and cache every tile's native size.
 * @param[in] r Reader.
 * @return Status.
 */
static int compute_tiles(int r);

/**
 * @brief Unrelated helper standing between the declaration and the body.
 * @param[in] v Value.
 * @return Value.
 */
static int passthrough(int v) { return v; }

static int compute_tiles(int r) { return r; }
""",
        {"DOC006"},
    ),
    (
        "-Wmissing-prototypes idiom: local prototype directly above its definition",
        """
/**
 * @brief Non-maskable interrupt handler.
 * @return Nothing.
 */
void NMI_Handler(void);
void NMI_Handler(void) { }
""",
        set(),
    ),
    (
        "banned pointer-only boilerplate",
        """
/** @brief Implementation of ra8_foo (see header for full contract). */
int ra8_foo(void) { return 0; }
""",
        {"DOC007"},
    ),
    # ---------------- must NOT fire ----------------
    (
        "correct function block",
        """
/**
 * @brief Add two numbers.
 * @param[in] lhs Left operand.
 * @param[in] rhs Right operand.
 * @return The sum.
 * @retval 0 Both operands were zero.
 */
int add_values(int lhs, int rhs) { return lhs + rhs; }
""",
        set(),
    ),
    (
        "correct void function (no @return/@retval)",
        """
/**
 * @brief Reset the widget.
 * @param[in] id Widget id.
 */
void widget_reset(int id) { (void)id; }
""",
        set(),
    ),
    (
        "@file block directly above a symbol block",
        """
/**
 * @file demo.c
 * @brief Demo translation unit.
 */
/**
 * @brief Add two numbers.
 * @param[in] lhs Left.
 * @param[in] rhs Right.
 * @return Sum.
 */
int add_values(int lhs, int rhs) { return lhs + rhs; }
""",
        set(),
    ),
    (
        "@defgroup and @{ grouping markers between blocks",
        """
/**
 * @defgroup lcd LCD driver
 * @{
 */
/**
 * @brief Clear the framebuffer.
 * @return Status.
 */
int lcd_clear(void);
/** @} */
""",
        set(),
    ),
    (
        "@copydoc block with another symbol's parameter names",
        """
/** @copydoc ra8_gpio_output_init */
int ra8_gpio_output_init_impl(int port, int pin) { return port + pin; }
""",
        set(),
    ),
    (
        "sanctioned definition-site single-line form, correct name",
        """
/** @brief Implementation of `ra8_err_to_str()` -- linear-scan lookup. */
int ra8_err_to_str(int code) { return code; }
""",
        set(),
    ),
    (
        "undocumented parameters with no @param at all (doxy_audit's job, not ours)",
        """
/**
 * @brief Add two numbers.
 * @return Sum.
 */
int add_values(int lhs, int rhs) { return lhs + rhs; }
""",
        set(),
    ),
    (
        "@param inside a @code example naming other symbols",
        """
/**
 * @brief Register a handler.
 * @param[in] handler Callback.
 * @return Status.
 * @code
 * // @param[in] port Port identifier
 * ra8_isr_register(handler);
 * @endcode
 */
int ra8_isr_register(int handler) { return handler; }
""",
        set(),
    ),
    (
        "variadic function documenting only its named parameters",
        """
/**
 * @brief Formatted log.
 * @param[in] fmt Format string.
 * @return Bytes written.
 */
int ra8_logf(const char* fmt, ...) { (void)fmt; return 0; }
""",
        set(),
    ),
    (
        "forward declaration bare, definition documented (the correct shape)",
        """
static int compute_tiles(int r);

/**
 * @brief Probe and cache every tile's native size.
 * @param[in] r Reader.
 * @return Status.
 */
static int compute_tiles(int r) { return r; }
""",
        set(),
    ),
    (
        "header declaration documented, definition bare (CLAUDE.md's prescribed split)",
        """
/**
 * @brief Add two numbers.
 * @param[in] lhs Left.
 * @param[in] rhs Right.
 * @return Sum.
 */
int add_values(int lhs, int rhs);
""",
        set(),
    ),
    (
        "namesake statics in different files must not merge (keyed per file)",
        """
/**
 * @brief Zero a buffer.
 * @param[in] len Length.
 */
static void internal_zero_bytes(int len) { (void)len; }
""",
        set(),
    ),
    (
        "pointer-back note WITH a real implementation note is allowed",
        """
/** @brief Implementation of `ra8_foo()` -- O(1) table lookup, see HUM Ch 5.2. */
int ra8_foo(void) { return 0; }
""",
        set(),
    ),
    (
        "\\def block documenting a deliberately commented-out config option",
        """
/**
 * \\def MBEDTLS_AES_ROM_TABLES
 *
 * Use precomputed AES tables stored in ROM.
 */
//#define MBEDTLS_AES_ROM_TABLES

/**
 * \\def MBEDTLS_AES_FEWER_TABLES
 *
 * Use less ROM/RAM for AES tables.
 */
//#define MBEDTLS_AES_FEWER_TABLES
""",
        set(),
    ),
    (
        "@var block stranded above another symbol's block, real variable left bare",
        """
/**
 * @var g_release_err
 * @brief Captured release code.
 */
/** @brief Sentinel for the release code. */
typedef enum : unsigned {
  k_err_none = 0U, /**< None yet. */
} err_sentinel_t;

volatile unsigned g_release_err = 0U;
""",
        {"DOC004"},
    ),
    (
        "'@return This function never returns.' on a [[noreturn]] void handler",
        """
/**
 * @brief Park the core forever.
 * @return This function never returns.
 */
[[noreturn]] void park_forever(void) { for (;;) { } }
""",
        set(),
    ),
    (
        "@retval on a [[noreturn]] void handler is still a contradiction",
        """
/**
 * @brief Park the core forever.
 * @return This function never returns.
 * @retval (none) The core spins in place.
 */
[[noreturn]] void park_forever(void) { for (;;) { } }
""",
        {"DOC003"},
    ),
    (
        "house-style '@return Nothing.' on a void function is not a contradiction",
        """
/**
 * @brief Reset the widget.
 * @param[in] id Widget id.
 * @return Nothing.
 */
void widget_reset(int id) { (void)id; }
""",
        set(),
    ),
    (
        "typedef'd anonymous struct named by its @struct tag (the C23 house shape)",
        """
/**
 * @struct sim_args_t
 * @brief Parsed command line.
 */
typedef struct {
  int verbose; /**< Verbosity level. */
} sim_args_t;
""",
        set(),
    ),
    (
        "typedef'd anonymous enum named by its @enum tag",
        """
/**
 * @enum lcd_state_t
 * @brief Panel states.
 */
typedef enum : unsigned char {
  k_lcd_state_idle = 0, /**< Idle. */
} lcd_state_t;
""",
        set(),
    ),
    (
        "struct with correctly-named @struct tag and documented members",
        """
/**
 * @struct panel_timing_t
 * @brief Panel timing.
 */
struct panel_timing_t {
  int hsync; /**< Horizontal sync width. */
  int vsync; /**< Vertical sync width. */
};
""",
        set(),
    ),
]


def selftest() -> int:
    """Run the synthetic fixtures in both directions."""
    cindex = _require_libclang()
    args = ["-std=c23", "-x", "c", "-DRA8_HOST_BUILD=1"]
    failures = 0
    with tempfile.TemporaryDirectory() as td:
        for idx, (name, src, expected) in enumerate(SELFTEST_CASES):
            path = Path(td) / f"case_{idx:02d}.c"
            path.write_text(src, encoding="ascii")
            rel = str(path)
            text = path.read_text(encoding="ascii")
            got: list[Finding] = check_consecutive_blocks(rel, text)
            got += check_banned_boilerplate(rel, text)
            index = cindex.Index.create()
            tu = index.parse(str(path), args=args)
            own = os.path.realpath(str(path))
            interesting = {
                cindex.CursorKind.FUNCTION_DECL,
                cindex.CursorKind.STRUCT_DECL,
                cindex.CursorKind.UNION_DECL,
                cindex.CursorKind.ENUM_DECL,
                cindex.CursorKind.TYPEDEF_DECL,
                cindex.CursorKind.FIELD_DECL,
                cindex.CursorKind.ENUM_CONSTANT_DECL,
                cindex.CursorKind.VAR_DECL,
            }
            anon_names = typedef_names_for_anonymous(tu, cindex, own)
            attached = blocks_by_attach_line(text)
            seen: set[tuple[int, str]] = set()
            for cursor in tu.cursor.walk_preorder():
                if cursor.kind not in interesting:
                    continue
                loc = cursor.location.file
                if loc is None or os.path.realpath(loc.name) != own:
                    continue
                key = (cursor.location.line, cursor.spelling or "")
                if key in seen:
                    continue
                seen.add(key)
                got += check_symbol(cursor, cindex, rel, anon_names, attached)
            got += check_forward_decl_blocks(tu, cindex, rel, own, text)

            codes = {f.code for f in got}
            if codes != expected:
                failures += 1
                sys.stderr.write(
                    f"  FAIL [{idx:02d}] {name}\n"
                    f"       expected {sorted(expected) or '<clean>'}\n"
                    f"       got      {sorted(codes) or '<clean>'}\n"
                )
                for f in got:
                    sys.stderr.write(f"         {f.code} {f.symbol}: {f.detail}\n")

    if failures:
        sys.stderr.write(
            f"check_doc_attachment.py: selftest FAILED ({failures}/{len(SELFTEST_CASES)} cases).\n"
        )
        return 2
    fires = sum(1 for _, _, e in SELFTEST_CASES if e)
    clean = len(SELFTEST_CASES) - fires
    print(
        f"check_doc_attachment.py: selftest passed "
        f"({len(SELFTEST_CASES)} cases: {fires} must-fire, {clean} must-not-fire)."
    )
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="CI gate: exit 1 on any finding")
    ap.add_argument("--selftest", action="store_true", help="run the synthetic fixtures")
    ap.add_argument("paths", nargs="*", help="files/dirs to scan (default: every first-party root)")
    ns = ap.parse_args()

    if ns.selftest:
        return selftest()

    if ns.paths:
        targets = [Path(p).resolve() for p in ns.paths]
    else:
        targets = [REPO_ROOT / r for r in SCAN_ROOTS if (REPO_ROOT / r).is_dir()]
    return run(targets, strict=ns.check)


if __name__ == "__main__":
    sys.exit(main())
