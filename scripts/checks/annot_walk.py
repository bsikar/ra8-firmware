# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Turning a parsed translation unit into a symbol table and a call list.

One pass over the AST fills three things the rules then reason over: the
USR-keyed symbol table, every call site, and the set of functions a hardware
vector table names.

Identity is the whole game here
-------------------------------
Symbols are keyed by USR (:func:`symbol_key`), never by name.  C lets every
translation unit have its own ``static`` helper with the same name and this
repo does exactly that -- three unrelated ``internal_zero_bytes`` live in
net_pal, usb_hmsc and usb_pmsc.  A name-keyed table merges them into one entry
whose annotations are the union of all three, so an annotation on one namesake
gets enforced against another's callers.  That is not hypothetical: it took
``dev`` red, and the selftest still reproduces the exact walk order that did it.

Call sites record the USR on *both* ends for the same reason.  A callee-keyed
lookup that used names would attribute one module's RA8_PRIV tag to another
module's file-local helper of the same name.
"""

from __future__ import annotations

import contextlib
import pathlib
import re

from annot_clang import SECTION_ATTR_KIND, cindex
from annot_model import AnnotatedSymbol, CallSite, ParseStats
from annot_rulekeys import ANNOTATION_PREFIXES
from annot_source import source_lines

#: A section name the linker script maps to a hardware vector table.
#: ``.vectors`` for the M85 images, ``.cpu1_vectors`` for the M33 ones.
_VECTOR_SECTION_RE = re.compile(r'section\s*\(\s*"(\.[a-z0-9_]*vectors)"')

#: How many lines above a declaration to search for its section attribute.
VECTOR_ATTR_LOOKBACK_LINES = 3


def collect_annotations(cursor: cindex.Cursor) -> list[str]:
    """Return every ra8_* string attached to a cursor via AnnotateAttr."""
    out: list[str] = []
    for child in cursor.get_children():
        if child.kind == cindex.CursorKind.ANNOTATE_ATTR:
            text = child.spelling or child.displayname or ""
            if text.startswith(ANNOTATION_PREFIXES):
                out.append(text)
    return out


def usr_of(cursor: cindex.Cursor) -> str:
    """Return the canonical USR of ``cursor``, or '' when unavailable."""
    with contextlib.suppress(Exception):
        return cursor.canonical.get_usr()
    return ""


def symbol_key(cursor: cindex.Cursor) -> str:
    """Return the symbol-table key for a function cursor.

    The USR when libclang can produce one -- it encodes the defining file
    for internal-linkage symbols, which is the only thing that separates
    same-named ``static`` helpers in different modules. A libclang build
    that cannot produce USRs degrades to name plus file, which keeps the
    namesakes apart for the common case rather than merging them.
    """
    usr = usr_of(cursor)
    if usr:
        return usr
    where = str(cursor.location.file) if cursor.location.file else ""
    return f"{cursor.spelling}@{where}"


def _file_of(cursor: cindex.Cursor) -> str:
    """Return the path holding ``cursor``, or '' when it has no location."""
    if cursor.location.file is None:
        return ""
    return str(cursor.location.file)


def _has_vector_section(cursor: cindex.Cursor) -> bool:
    """True when ``cursor``'s declaration carries a ``*vectors`` section."""
    if cursor.location.file is None:
        return False
    lines = source_lines(str(cursor.location.file))
    if not lines:
        return False
    # The attribute sits on the declaration line or just above it; C23
    # attributes may be split across a couple of lines by the formatter.
    first = max(0, cursor.extent.start.line - VECTOR_ATTR_LOOKBACK_LINES)
    return bool(_VECTOR_SECTION_RE.search("\n".join(lines[first : cursor.extent.start.line])))


def is_vector_table(cursor: cindex.Cursor) -> bool:
    """True when ``cursor`` declares a hardware exception vector table.

    Recognised by what makes it a vector table, never by name. Either:

    * a file-scope array of ``const`` pointers to functions -- the shape
      the Cortex-M core reads through VTOR, which nothing else in this
      tree has; or
    * a file-scope ``const`` array the linker places in a ``*vectors``
      section. The two-entry copy-to-run payload table and the M33
      ``.cpu1_vectors`` table are ``const uintptr_t[]``, so the type alone
      does not identify them, but the section placement does -- that
      placement is the whole reason the array exists.

    The section is read out of the declaration's source text because
    ``CursorKind.SECTION_ATTR`` is absent from the libclang 18.1.x wheels
    the runners install, so an AST lookup would silently find nothing.
    """
    if cursor.kind != cindex.CursorKind.VAR_DECL:
        return False
    parent = cursor.semantic_parent
    if parent is None or parent.kind != cindex.CursorKind.TRANSLATION_UNIT:
        return False
    array = cursor.type
    if array.kind not in (cindex.TypeKind.CONSTANTARRAY, cindex.TypeKind.INCOMPLETEARRAY):
        return False
    element = array.element_type
    if not element.is_const_qualified():
        return False
    canonical = element.get_canonical()
    if (
        canonical.kind == cindex.TypeKind.POINTER
        and canonical.get_pointee().get_canonical().kind == cindex.TypeKind.FUNCTIONPROTO
    ):
        return True
    return _has_vector_section(cursor)


class _Walker:
    """One traversal, accumulating into the caller's collections.

    A class rather than a nest of closures so each construct the walk knows
    about -- function declaration, call, address-of, vector table -- is a
    separately readable method instead of another branch in one long visitor.
    """

    def __init__(
        self,
        symbols: dict[str, AnnotatedSymbol],
        calls: list[CallSite],
        stats: ParseStats,
        vector_entries: set[str],
    ) -> None:
        self.symbols = symbols
        self.calls = calls
        self.stats = stats
        self.vector_entries = vector_entries

    def _record_vector_entries(self, node: cindex.Cursor) -> None:
        """Collect the USR of every function named in a vector table."""
        if (
            node.kind == cindex.CursorKind.DECL_REF_EXPR
            and node.referenced is not None
            and node.referenced.kind == cindex.CursorKind.FUNCTION_DECL
        ):
            self.vector_entries.add(symbol_key(node.referenced))
        for child in node.get_children():
            self._record_vector_entries(child)

    def _symbol_for(self, node: cindex.Cursor) -> AnnotatedSymbol:
        """Return (creating if needed) the table entry for a function cursor."""
        key = symbol_key(node)
        return self.symbols.setdefault(
            key,
            AnnotatedSymbol(
                name=node.spelling, file=_file_of(node), line=node.location.line, usr=key
            ),
        )

    @staticmethod
    def _update_declaration(sym: AnnotatedSymbol, node: cindex.Cursor) -> None:
        """Fold one declaration or definition of a function into its entry."""
        for a in collect_annotations(node):
            if a not in sym.annotations:
                sym.annotations.append(a)
        sym.is_static = node.linkage == cindex.LinkageKind.INTERNAL or sym.is_static
        sym.return_type = node.result_type.spelling
        if any(arg.type.kind == cindex.TypeKind.POINTER for arg in node.get_arguments()):
            sym.has_pointer_param = True
        if node.is_definition():
            sym.is_defined = True
            sym.file = _file_of(node)
            sym.line = node.location.line
            sym.end_line = node.extent.end.line
        else:
            sym.decl_files.add(_file_of(node))
        # libclang exposes inline-ness via the cursor's tokens
        tokens = [t.spelling for t in node.get_tokens()]
        if "inline" in tokens or "__inline__" in tokens:
            sym.has_inline = True
        # Section attribute (only when this libclang exposes the kind).
        if SECTION_ATTR_KIND is not None:
            for child in node.get_children():
                if child.kind == SECTION_ATTR_KIND:
                    sym.section = child.spelling

    def _visit_function(self, node: cindex.Cursor, current_func: cindex.Cursor | None) -> None:
        """Record a function declaration, then descend into its body."""
        self._update_declaration(self._symbol_for(node), node)
        inner = node if node.is_definition() else current_func
        for child in node.get_children():
            self.visit(child, inner)

    def _record_call(self, node: cindex.Cursor, current_func: cindex.Cursor) -> None:
        """Record one direct call, and whether its callee resolved."""
        self.stats.calls_seen += 1
        callee = node.referenced
        if callee is None:
            return
        self.stats.calls_resolved += 1
        self.calls.append(
            CallSite(
                callee_name=callee.spelling,
                caller_name=current_func.spelling,
                caller_file=_file_of(node),
                caller_line=node.location.line,
                callee_usr=symbol_key(callee),
                caller_usr=symbol_key(current_func),
            )
        )

    def _record_address_of(self, node: cindex.Cursor, current_func: cindex.Cursor | None) -> None:
        """Record ``&foo`` references, which are uses but not direct calls."""
        tokens = [t.spelling for t in node.get_tokens()]
        if not tokens or tokens[0] != "&":
            return
        self.calls.extend(
            CallSite(
                callee_name=child.referenced.spelling,
                caller_name=(current_func.spelling if current_func else ""),
                caller_file=_file_of(node),
                caller_line=node.location.line,
                callee_usr=symbol_key(child.referenced),
                caller_usr=(symbol_key(current_func) if current_func else ""),
                in_address_of=True,
            )
            for child in node.get_children()
            if (
                child.kind == cindex.CursorKind.DECL_REF_EXPR
                and child.referenced
                and child.referenced.kind == cindex.CursorKind.FUNCTION_DECL
            )
        )

    def visit(self, node: cindex.Cursor, current_func: cindex.Cursor | None) -> None:
        """Dispatch one cursor, then recurse into its children."""
        if node.kind == cindex.CursorKind.FUNCTION_DECL:
            self._visit_function(node, current_func)
            return

        if is_vector_table(node):
            self._record_vector_entries(node)

        if node.kind == cindex.CursorKind.CALL_EXPR and current_func is not None:
            self._record_call(node, current_func)

        if node.kind == cindex.CursorKind.UNARY_OPERATOR:
            self._record_address_of(node, current_func)

        for child in node.get_children():
            self.visit(child, current_func)


def walk_tu(
    tu: cindex.TranslationUnit,
    _tu_path: pathlib.Path,
    symbols: dict[str, AnnotatedSymbol],
    calls: list[CallSite],
    stats: ParseStats,
    vector_entries: set[str],
) -> None:
    """Single-pass AST walk filling ``symbols``, ``calls`` and ``vector_entries``."""
    _Walker(symbols, calls, stats, vector_entries).visit(tu.cursor, None)
