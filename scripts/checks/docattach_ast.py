# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""The checks that need a real parse, and the libclang setup they need it from.

Five of this gate's seven findings are questions no regex can answer: whether
an ``@param`` names a parameter the signature actually has, whether a function
returning ``void`` claims a return value, whether a block names a different
symbol than the one it sits on, and whether a block is stranded on a forward
declaration whose definition is bare.  Each needs real parameter names, real
return types and real declaration-vs-definition identity.

That dependency is also why the import lives here and fails loudly: a gate
that silently degrades to "no findings" when libclang is missing is worse than
one that is not run at all, because it reports success.
"""

from __future__ import annotations

import os
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from types import ModuleType
from typing import TYPE_CHECKING

from docattach_lex import (
    _blank_comments_and_literals,
    check_banned_boilerplate,
    check_consecutive_blocks,
    extract_doc_blocks,
)
from docattach_model import DocBlock, DocTags, Finding, parse_tags
from docattach_scope import REPO_ROOT

if TYPE_CHECKING:  # pragma: no cover -- libclang is imported at runtime by _require_libclang
    from clang.cindex import Cursor, TranslationUnit

#: A symbol must appear at least this many times (declaration + definition)
#: before a forward-declaration-vs-definition split can even exist.
MIN_REDECLARATIONS = 2


# ---------------------------------------------------------------------------
# libclang
# ---------------------------------------------------------------------------
def _require_libclang() -> ModuleType:
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


def _include_args(cindex: ModuleType) -> list[str]:  # noqa: ARG001  # kept for signature parity
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
# AST pass
# ---------------------------------------------------------------------------
def _param_names(cursor: Cursor, cindex: ModuleType) -> list[str]:
    """Named parameters of a function cursor, in order."""
    return [
        a.spelling
        for a in cursor.get_arguments()
        if a.kind == cindex.CursorKind.PARM_DECL and a.spelling
    ]


def _is_macro_expanded(cursor: Cursor) -> bool:
    """True when the declaration came out of a macro expansion.

    X-macro tables and declaration-generating macros produce cursors whose
    spelling never appears in the source text, so parameter and name checks
    against them are meaningless.
    """
    try:
        return cursor.location.is_from_main_file() is False and cursor.extent.start.file is None
    except (AttributeError, ValueError):
        return False


def typedef_names_for_anonymous(
    tu: TranslationUnit, cindex: ModuleType, own_file: str
) -> dict[tuple[int, int], str]:
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


def _display_name(cursor: Cursor, anon_names: dict[tuple[int, int], str]) -> str:
    """The name a doc block would reasonably use for ``cursor``."""
    spelling = cursor.spelling or ""
    if spelling and "(unnamed" not in spelling and "(anonymous" not in spelling:
        return spelling
    return anon_names.get((cursor.location.line, cursor.location.column), "")


@dataclass
class FileCtx:
    """Everything the per-symbol checks need about the file being scanned."""

    cindex: object
    path: str
    #: Anonymous record/enum location -> the typedef that names it.
    anon_names: dict[tuple[int, int], str]
    #: Source line of a declaration -> the doc block that precedes it.
    attached: dict[int, DocBlock]
    #: The file's source lines, for span-level lookups.
    decl_text: list[str]


def _check_names(
    cursor: Cursor, ctx: FileCtx, tags: DocTags, name: str, line: int
) -> list[Finding]:
    """DOC005 -- the block names one symbol while sitting on another."""
    findings: list[Finding] = []
    for kind, ref in tags.explicit_refs:
        if kind in {"ingroup", "name"} or ref == name:
            continue
        # The block names X; if X appears anywhere in the declaration the block
        # sits on, it IS attached to the right thing and libclang simply named
        # the cursor badly. `typedef UINT (*dfu_write_cb_t)(...)` is the case
        # that matters: with the vendored USBX headers unresolved, clang
        # recovers by reporting the cursor as `UINT`, which looked like a
        # mismatch against a perfectly correct @typedef dfu_write_cb_t.
        if _ref_in_declaration(cursor, ref, ctx.decl_text):
            continue
        findings.append(
            Finding(
                ctx.path,
                line,
                "DOC005",
                name,
                f"block says '@{kind} {ref}' but is attached to '{name}'",
            )
        )
        break

    # The sanctioned definition-site form naming a different function.
    if tags.impl_of and tags.impl_of != name:
        findings.append(
            Finding(
                ctx.path,
                line,
                "DOC005",
                name,
                f"block says 'Implementation of `{tags.impl_of}()`' but is attached to '{name}'",
            )
        )
    return findings


def check_symbol(cursor: Cursor, ctx: FileCtx) -> list[Finding]:
    """Run every attachment check for one documented declaration.

    Attachment is resolved **positionally** (``attached``), never via
    ``cursor.raw_comment``.  libclang propagates a comment to every
    redeclaration of a symbol, so a block on a forward declaration also reports
    as the definition's own comment -- which double-reported every finding and
    would have made the eventual fix look incomplete.  The block a reader sees
    above a declaration is the one this gate judges.
    """
    cindex = ctx.cindex
    findings: list[Finding] = []
    block = _decl_block(cursor, ctx.attached, ctx.decl_text)
    if block is None:
        return findings
    tags = parse_tags(block.text)
    name = _display_name(cursor, ctx.anon_names)
    line = cursor.location.line
    # An anonymous record with no naming typedef has no name a block could be
    # checked against; the tag checks below would compare against "".
    if not name:
        return findings

    # `@copydoc` substitutes another symbol's block wholesale at render time,
    # so its tags describe that symbol by design. Nothing to cross-check.
    if tags.has_copy:
        return findings

    findings.extend(_check_names(cursor, ctx, tags, name, line))

    if cursor.kind != cindex.CursorKind.FUNCTION_DECL:
        return findings
    findings.extend(_check_signature(cursor, ctx, tags, name, line))
    return findings


def _check_signature(
    cursor: Cursor, ctx: FileCtx, tags: DocTags, name: str, line: int
) -> list[Finding]:
    """DOC001/DOC002/DOC003 -- the block's claims against the real signature."""
    cindex, path = ctx.cindex, ctx.path
    findings: list[Finding] = []
    actual = _param_names(cursor, cindex)
    documented = [p for p in tags.params if p]

    # -- DOC001: a documented parameter the signature does not have.
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
    missing = [p for p in actual if p not in documented] if documented else []
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


#: Lines that belong to a declaration but sit *above* the extent libclang
#: reports: the RA8_* annotation macros (which expand to attributes and are
#: mandated tree-wide by CLAUDE.md), C23 attributes, and bare storage-class or
#: qualifier keywords left on their own line by clang-format.
DECL_PREFIX_RE = re.compile(
    r"^\s*(?:"
    r"[A-Z][A-Z0-9_]*(?:\s*\([^)]*\))?"  # RA8_INTERNAL / RA8_BOUNDED_LOOP(x)
    r"|\[\[[^\]]*\]\]"  # [[noreturn]]
    r"|static|inline|extern|const|volatile|register|_Noreturn"
    r")\s*$"
)


def _decl_block(cursor: Cursor, attached: dict[int, DocBlock], lines: list[str]) -> DocBlock | None:
    """The doc block lexically preceding ``cursor``, or None.

    A declaration can start well above ``cursor.location.line``.  libclang's
    extent does **not** cover an annotation macro on its own line, so for

        /** ... */
        RA8_INTERNAL
        static void internal_foo(void) { }

    the block attaches to the ``RA8_INTERNAL`` line while the cursor extent
    starts on the ``static void`` line below it -- and a naive extent-based
    lookup finds no block at all.  Since CLAUDE.md mandates an RA8_* annotation
    on every non-public function, that silently exempted a large share of the
    tree from every symbol check here.  Walk up past any declaration-prefix
    lines before looking.
    """
    try:
        first = cursor.extent.start.line
    except (AttributeError, ValueError):
        first = cursor.location.line
    first = min(first, cursor.location.line)
    while first > 1 and DECL_PREFIX_RE.match(lines[first - 2] if first - 2 < len(lines) else ""):
        first -= 1
    for ln in range(first, cursor.location.line + 1):
        if ln in attached:
            return attached[ln]
    return None


def _ref_in_declaration(cursor: Cursor, ref: str, decl_text: list[str]) -> bool:
    """True when ``ref`` appears in the source lines ``cursor`` spans."""
    try:
        lo = cursor.extent.start.line
        hi = cursor.extent.end.line
    except (AttributeError, ValueError):
        return False
    pat = re.compile(r"\b" + re.escape(ref) + r"\b")
    return any(pat.search(ln) for ln in decl_text[lo - 1 : hi])


def _decls_between(all_decls: list[Cursor], first: Cursor, second: Cursor) -> bool:
    """True when some *other* function is declared between ``first`` and ``second``."""
    lo, hi = first.location.line, second.location.line
    return any(lo < c.location.line < hi and c.spelling != first.spelling for c in all_decls)


def check_forward_decl_blocks(
    tu: TranslationUnit, cindex: ModuleType, path: str, own_file: str, text: str
) -> list[Finding]:
    """DOC006 -- a block on a forward declaration whose definition is bare.

    Scoped to a single file on purpose.  A block on a *header* declaration with
    a bare definition in the .c is exactly what CLAUDE.md prescribes; the defect
    is the in-file forward declaration that hoards the block and leaves the
    definition below it undocumented.
    """
    attached = blocks_by_attach_line(text)
    decl_text = text.splitlines()
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
        if _decl_block(definition, attached, decl_text) is not None:
            continue
        documented_decl = next(
            (
                c
                for c in cursors
                if not c.is_definition() and _decl_block(c, attached, decl_text) is not None
            ),
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


def check_file(path: Path, cindex: ModuleType, args: list[str]) -> list[Finding]:
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
    findings.extend(_check_declarations(tu, cindex, rel, own, text))
    findings.extend(check_forward_decl_blocks(tu, cindex, rel, own, text))
    return _dedupe(findings)


def _check_declarations(
    tu: TranslationUnit, cindex: ModuleType, rel: str, own: str, text: str
) -> list[Finding]:
    """Run the per-symbol checks over every declaration this file owns."""
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
    ctx = FileCtx(cindex, rel, anon_names, blocks_by_attach_line(text), text.splitlines())
    findings: list[Finding] = []
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
        findings.extend(check_symbol(cursor, ctx))
    return findings


def _dedupe(findings: list[Finding]) -> list[Finding]:
    """Collapse identical findings reported at two cursors.

    One doc block above ``typedef enum {...} foo_t;`` is reached by both the
    ENUM_DECL and the TYPEDEF_DECL cursor, which reports the identical finding
    at two different lines (the ``typedef enum {`` line and the ``} foo_t;``
    line). Keeping the first occurrence makes a fix count match the defect
    count.
    """
    deduped: dict[tuple[str, str, str], Finding] = {}
    for f in findings:
        deduped.setdefault((f.code, f.symbol, f.detail), f)
    return list(deduped.values())
