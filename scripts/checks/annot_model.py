# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""The data ``check_annotations.py`` builds from a parse and then reasons over.

Four records, no behaviour: the symbol table entry, a call site, the evidence
that the parse actually happened, and a finding.  They live apart from both
the walk that fills them and the rules that read them so that neither side can
quietly redefine the other's shape -- and so the rules stay testable without
libclang present.
"""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass
class AnnotatedSymbol:
    """One function, keyed by USR, with whatever ra8_* annotations it carries.

    Every function declaration and definition the walk sees lands here,
    annotated or not: the linkage rule has to reason about the functions
    that carry *no* annotation, which is precisely the set an
    annotations-only table cannot see.

    The table is keyed by USR rather than by name. C lets every
    translation unit have its own ``static`` helper with the same name,
    and this repo does exactly that -- three unrelated
    ``internal_zero_bytes`` live in net_pal, usb_hmsc and usb_pmsc. A
    name-keyed table merges them into one entry whose annotations are the
    union of all three and whose file is whichever definition happened to
    be parsed last, so an annotation on one namesake is enforced against
    the callers of another.
    """

    name: str
    file: str
    line: int
    annotations: list[str] = field(default_factory=list)
    is_static: bool = False
    has_inline: bool = False
    section: str | None = None
    return_type: str = ""
    #: Last source line of the definition's extent (0 when only declared).
    end_line: int = 0
    #: True when at least one parameter is a pointer, i.e. the function
    #: takes an address across the boundary and must range-check it.
    has_pointer_param: bool = False
    #: Clang Unified Symbol Resolution string of the canonical declaration.
    #: This is the only reliable identity for a C function: two `static`
    #: helpers in different TUs may share a name but never a USR.
    usr: str = ""
    #: True once a definition (not just a prototype) has been seen.
    is_defined: bool = False
    #: Every file holding a non-defining declaration of this function.
    #: The linkage rule reads the published interface off this set: a
    #: prototype in a header is an exported contract, a prototype in a
    #: `.c` is a forward declaration and exports nothing.
    decl_files: set[str] = field(default_factory=set)


@dataclass
class CallSite:
    """A direct CallExpr discovered during AST traversal."""

    callee_name: str
    caller_name: str
    caller_file: str
    caller_line: int
    #: USR of the declaration this call actually resolved to. Compared
    #: against AnnotatedSymbol.usr so an annotation on one function is
    #: never attributed to a same-named `static` in another module.
    callee_usr: str = ""
    #: USR of the function the call sits inside, so caller-side lookups
    #: separate namesake `static` helpers the same way callee lookups do.
    caller_usr: str = ""
    in_address_of: bool = False
    parent_loop_label: str | None = None  # for RA8_PROTECTED_WRITE detection
    preceding_comment: str = ""


@dataclass
class ParseStats:
    """Evidence that the parse saw the code it was asked to analyse."""

    #: Every CallExpr encountered inside a function body.
    calls_seen: int = 0
    #: Those whose callee declaration libclang could resolve.
    calls_resolved: int = 0
    #: ``(including file, missing header)`` for each unresolved include.
    missing_includes: set[tuple[str, str]] = field(default_factory=set)
    #: Translation units libclang refused to parse at all.
    unparsed: list[str] = field(default_factory=list)


@dataclass
class Violation:
    """One finding. ``warn_only`` entries are reported but never fail the gate."""

    rule: str
    file: str
    line: int
    message: str
    warn_only: bool = False
