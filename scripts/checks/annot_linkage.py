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

from annot_model import AnnotatedSymbol, DataSymbol, Violation
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

#: Prefixes are linkage vocabulary, not interchangeable Hungarian notation.
STATIC_FUNCTION_PREFIX = "internal_"
PRIVATE_FUNCTION_PREFIX = "priv_"
STATIC_DATA_PREFIX = "s_"


def _is_published_inline_definition(sym: AnnotatedSymbol) -> bool:
    """True for a static inline API body intentionally defined in public ``inc``."""
    path = pathlib.PurePath(relative(sym.file))
    return (
        path.suffix in {".h", ".hpp"}
        and INTERNAL_HEADER_SUFFIX not in path.name
        and "inc" in path.parts
        and sym.has_inline
    )


def _linkage_keys(sym: AnnotatedSymbol) -> set[str]:
    """Return the linkage annotations attached to ``sym``."""
    return {
        parse_annotation(annotation)[0]
        for annotation in sym.annotations
        if parse_annotation(annotation)[0] in LINKAGE_ANNOTATIONS
    }


def _naming_at(sym: AnnotatedSymbol, message: str) -> Violation:
    """Build one function naming finding at its definition site."""
    return Violation("ra8_naming", sym.file, sym.line, message)


def _annotation_shape(sym: AnnotatedSymbol, linkage: set[str]) -> list[Violation]:
    """Check that every linkage annotation agrees with storage and prefix."""
    out: list[Violation] = []
    if len(linkage) > 1:
        out.append(
            _naming_at(
                sym,
                f"function '{sym.name}' carries conflicting linkage annotations: "
                f"{', '.join(sorted(linkage))}",
            )
        )
    if "ra8_internal" in linkage and not sym.name.startswith(STATIC_FUNCTION_PREFIX):
        out.append(
            _naming_at(sym, f"RA8_INTERNAL function '{sym.name}' must use the internal_ prefix")
        )
    if "ra8_priv" in linkage and sym.is_static:
        out.append(_naming_at(sym, f"RA8_PRIV function '{sym.name}' must not be static"))
    if "ra8_priv" in linkage and not sym.name.startswith(PRIVATE_FUNCTION_PREFIX):
        out.append(_naming_at(sym, f"RA8_PRIV function '{sym.name}' must use the priv_ prefix"))
    if "ra8_test_helper" in linkage and sym.is_static:
        out.append(_naming_at(sym, f"RA8_TEST_HELPER function '{sym.name}' must not be static"))
    return out


def _static_shape(sym: AnnotatedSymbol, linkage: set[str]) -> list[Violation]:
    """Check a non-public file-local function's storage, annotation, and prefix."""
    if not sym.has_internal_linkage or _is_published_inline_definition(sym):
        return []
    out: list[Violation] = []
    if not sym.is_static:
        out.append(_naming_at(sym, f"file-local function '{sym.name}' must be declared static"))
    if "ra8_internal" not in linkage:
        out.append(_naming_at(sym, f"file-local function '{sym.name}' must carry RA8_INTERNAL"))
    if not sym.name.startswith(STATIC_FUNCTION_PREFIX):
        out.append(
            _naming_at(sym, f"file-local function '{sym.name}' must use the internal_ prefix")
        )
    return out


def _reserved_prefix_shape(sym: AnnotatedSymbol, linkage: set[str]) -> list[Violation]:
    """Reserve ``internal_`` and ``priv_`` for their exact linkage shapes."""
    out: list[Violation] = []
    if sym.name.startswith(STATIC_FUNCTION_PREFIX):
        if not sym.is_static:
            out.append(
                _naming_at(sym, f"function '{sym.name}' uses internal_ but is not declared static")
            )
        if "ra8_internal" not in linkage:
            out.append(
                _naming_at(sym, f"function '{sym.name}' uses internal_ but lacks RA8_INTERNAL")
            )
    if sym.name.startswith(PRIVATE_FUNCTION_PREFIX) and "ra8_priv" not in linkage:
        out.append(_naming_at(sym, f"function '{sym.name}' uses priv_ but lacks RA8_PRIV"))
    return out


def _private_declaration_shape(sym: AnnotatedSymbol, linkage: set[str]) -> list[Violation]:
    """Require an RA8_PRIV contract to be private and published nowhere else."""
    if "ra8_priv" not in linkage:
        return []
    out: list[Violation] = []
    if internal_header(sym) is None:
        out.append(
            _naming_at(
                sym,
                f"RA8_PRIV function '{sym.name}' has no declaration in a module "
                "*_internal.h",
            )
        )
    public_decl = published_header(sym)
    if public_decl is not None:
        out.append(
            _naming_at(
                sym,
                f"RA8_PRIV function '{sym.name}' is also published by "
                f"{relative(public_decl)}",
            )
        )
    return out


def _function_naming(sym: AnnotatedSymbol) -> list[Violation]:
    """Return every prefix/storage finding for one function definition."""
    linkage = _linkage_keys(sym)
    out: list[Violation] = []
    if sym.name.startswith(STATIC_DATA_PREFIX):
        out.append(
            _naming_at(
                sym,
                f"function '{sym.name}' uses the s_ prefix reserved for file-scope static data",
            )
        )
    out.extend(_annotation_shape(sym, linkage))
    out.extend(_static_shape(sym, linkage))
    out.extend(_reserved_prefix_shape(sym, linkage))
    out.extend(_private_declaration_shape(sym, linkage))
    return out


def _data_naming(data: DataSymbol) -> Violation | None:
    """Return the prefix/scope finding for one data definition, if any."""
    correctly_static = data.is_file_scope and data.has_internal_linkage
    if correctly_static and not data.name.startswith(STATIC_DATA_PREFIX):
        return Violation(
            "ra8_naming",
            data.file,
            data.line,
            f"file-scope static data '{data.name}' must use the s_ prefix",
        )
    if data.name.startswith(STATIC_DATA_PREFIX) and not correctly_static:
        return Violation(
            "ra8_naming",
            data.file,
            data.line,
            f"data '{data.name}' uses s_ but is not file-scope with internal linkage",
        )
    return None


def _naming_violations(
    symbols: dict[str, AnnotatedSymbol], data_symbols: dict[str, DataSymbol]
) -> list[Violation]:
    """Enforce and source-site-deduplicate the style-guide naming contract."""
    findings = [
        finding
        for sym in symbols.values()
        if sym.is_defined and is_first_party(sym.file)
        for finding in _function_naming(sym)
    ]
    findings.extend(
        finding
        for data in data_symbols.values()
        if is_first_party(data.file)
        if (finding := _data_naming(data)) is not None
    )
    unique: dict[tuple[str, int, str], Violation] = {}
    for finding in findings:
        unique.setdefault((finding.file, finding.line, finding.message), finding)
    return list(unique.values())


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
    data_symbols: dict[str, DataSymbol] | None = None,
    *,
    naming_contract: bool = False,
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
    out = _naming_violations(symbols, data_symbols or {}) if naming_contract else []
    verdicts: dict[tuple[str, int], list[Violation | None]] = {}
    for sym in symbols.values():
        if not sym.is_defined or sym.has_internal_linkage or not is_first_party(sym.file):
            continue
        verdicts.setdefault((sym.file, sym.line), []).append(_linkage_verdict(sym, vector_entries))
    for site in sorted(verdicts):
        found = [v for v in verdicts[site] if v is not None]
        if len(found) == len(verdicts[site]) and found:
            out.append(found[0])
    return out
