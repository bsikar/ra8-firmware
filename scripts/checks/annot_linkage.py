# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""The linkage rule: every non-static function states why it has external linkage.

Separate from the other rules because it is the only one defined over the
*absence* of an annotation.  Every rule in :mod:`annot_rules` starts from a tag
someone wrote and checks a property of it; this one starts from the whole
symbol table and asks which definitions nobody has classified at all.  That
makes it the rule most likely to look clean by simply not running -- so it also
owns the largest share of the selftest.
"""

from __future__ import annotations

import pathlib

from annot_model import AnnotatedSymbol, Violation
from annot_rulekeys import LINKAGE_ANNOTATIONS, parse_annotation
from annot_scope import SOURCE_SUFFIXES, is_first_party, relative

#: Header suffix that marks a declaration as library-private rather than
#: published API. See CLAUDE.md, "Which linkage annotation to use".
INTERNAL_HEADER_SUFFIX = "_internal.h"

#: The ISO C program entry point. The startup code reaches `main` by name
#: through the C runtime contract, so it has external linkage by
#: definition and no first-party header declares it. Exactly one name --
#: this is a language rule, not a naming convention.
C_ENTRY_POINT = "main"


def published_header(sym: AnnotatedSymbol) -> str | None:
    """Return the header that publishes ``sym``, or None when none does.

    A prototype in a header is an exported contract: a library's public
    ``inc/`` header, an application's local header, a mock's header, or a
    vendored SOUP header whose interface the function implements. A
    prototype in a ``.c`` is a forward declaration and publishes nothing,
    and an ``*_internal.h`` declaration is explicitly library-private --
    both leave the function needing a linkage annotation.

    "Header" is anything that is not a translation unit, rather than a
    list of header suffixes: the C++ standard library headers that declare
    the replaceable ``operator new`` / ``operator delete`` are spelled
    ``<new>``, with no suffix at all.
    """
    for path in sorted(sym.decl_files):
        name = pathlib.PurePath(path).name
        if pathlib.PurePath(name).suffix in SOURCE_SUFFIXES:
            continue
        if name.endswith(INTERNAL_HEADER_SUFFIX):
            continue
        return path
    return None


def internal_header(sym: AnnotatedSymbol) -> str | None:
    """Return the ``*_internal.h`` declaring ``sym``, or None."""
    for path in sorted(sym.decl_files):
        if pathlib.PurePath(path).name.endswith(INTERNAL_HEADER_SUFFIX):
            return path
    return None


def _linkage_verdict(sym: AnnotatedSymbol, vector_entries: set[str]) -> Violation | None:
    """Return the linkage violation for ``sym``, or None when it passes."""
    if any(parse_annotation(a)[0] in LINKAGE_ANNOTATIONS for a in sym.annotations):
        return None
    if published_header(sym) is not None:
        return None
    if sym.usr in vector_entries or sym.name == C_ENTRY_POINT:
        return None
    internal = internal_header(sym)
    if internal is not None:
        return Violation(
            "ra8_linkage",
            sym.file,
            sym.line,
            f"'{sym.name}' is declared in {relative(internal)} but carries no "
            f"linkage annotation; a symbol that is non-static only so one "
            f"library's other TUs can reach it is RA8_PRIV (or RA8_TEST_HELPER "
            f"when only tests call it)",
        )
    return Violation(
        "ra8_linkage",
        sym.file,
        sym.line,
        f"'{sym.name}' has external linkage but nothing publishes it: no header "
        f"declares it and no vector table names it. Make it static and tag it "
        f"RA8_INTERNAL, or declare it -- in the library's inc/ header if it is "
        f"API, in an *_internal.h with RA8_PRIV if it is shared inside the library",
    )


def enforce_linkage(
    symbols: dict[str, AnnotatedSymbol],
    vector_entries: set[str],
) -> list[Violation]:
    """Every non-static function must state why it has external linkage.

    CLAUDE.md, "Which linkage annotation to use", makes this a tree-wide
    expectation rather than an opt-in: a function that is not `static`
    either publishes a contract other code may call, or it is deliberately
    non-`static` for one narrow reason that has to be written down.

    A definition passes when any of the following holds.

    * It carries `RA8_PRIV`, `RA8_INTERNAL` or `RA8_TEST_HELPER`.
    * A header publishes it. A prototype in a `.h` is an exported
      contract -- a library's public `inc/` header, an application's local
      header, a mock's header, or the vendored SOUP header whose interface
      the function implements (the NimBLE `ble_npl_*` porting layer, the
      ThreadX `tx_application_define` hook, the USBX class entry points).
      An `*_internal.h` prototype does not count: that header exists to
      say "library-private", which is what `RA8_PRIV` marks.
    * It is a hardware vector-table entry -- see `is_vector_table()`. The
      CPU reaches these through VTOR with no C caller at all, so no
      annotation describes them and no header can publish them: the only
      thing that names an IRQ trampoline is the table slot itself. The
      category is derived from the table's structure, so a handler that is
      removed from the table stops being exempt the moment it is unwired.
    * It is `main`, the ISO C entry point.

    Anything else is a gap, and the two shapes need different fixes, so
    they get different messages.

    A verdict is reached per *definition site*, not per symbol name. The
    coverage tests reach a module's `static` helpers by `#define`-ing a
    function to a `_cov` spelling and then `#include`-ing the `.c`, so one
    source construct shows up under two names with two USRs and the same
    file and line. Judging the renamed copy on its own would report the
    original function as an unpublished symbol in a file that never
    declared it.
    """
    verdicts: dict[tuple[str, int], list[Violation | None]] = {}
    for sym in symbols.values():
        if not sym.is_defined or sym.is_static or not is_first_party(sym.file):
            continue
        verdicts.setdefault((sym.file, sym.line), []).append(_linkage_verdict(sym, vector_entries))
    out: list[Violation] = []
    for site in sorted(verdicts):
        found = [v for v in verdicts[site] if v is not None]
        if len(found) == len(verdicts[site]) and found:
            out.append(found[0])
    return out
