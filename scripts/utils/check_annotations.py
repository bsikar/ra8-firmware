#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""
check_annotations.py -- libclang-based annotation enforcement.

This script walks the AST of every C / C++ translation unit under
``libs/``, ``src/``, ``examples/``, ``tests/``, and ``port/`` and
applies the project's annotation-enforcement rules. The annotation
macros themselves are defined in ``libs/ra8_core/inc/ra8_attributes.h``
(produced by ) and lower to GCC ``__attribute__((annotate
("ra8_<rule>:...")))`` markers that libclang exposes via
``AnnotateAttr`` cursors.

The 19 enforceable rules are documented in ``docs/ANNOTATIONS.md``;
this script is the canonical implementation of their static checks.

For the script defaults to **warn-only** (mirrors the
``cite_check`` / ``check_world_tags`` pattern). Flip the
``WARN_ONLY_MODE`` flag at the top of the file to ``False`` once
the codebase clears the gate.

Usage::

    python3 scripts/utils/check_annotations.py            # warn-only
    python3 scripts/utils/check_annotations.py --check    # CI gate
    python3 scripts/utils/check_annotations.py --list     # dump symbols
"""

from __future__ import annotations

import argparse
import contextlib
import os
import pathlib
import re
import subprocess
import sys
from dataclasses import dataclass, field

# --------------------------------------------------------------------------
# warn-only gate. Flip to False to make violations fatal by default.
# --------------------------------------------------------------------------
WARN_ONLY_MODE = True


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]

SCAN_DIRS = ("libs", "src", "examples", "tests", "port")
EXCLUDED_PATH_PARTS = {
    "build",
    "_deps",
    "third_party",
    "build-cov",
    "build-bench",
    "build-scan",
    "build-mcdc",
}
SOURCE_SUFFIXES = {".c", ".cpp"}

WARN_ONLY_RULES = {"ra8_latency_max_ns", "ra8_reviewed_by", "ra8_register_bank"}

# Upper bound on the call-graph BFS used for ra8_no_recursion checking.
# Prevents infinite loops on pathological call graphs during static analysis.
RECURSION_GUARD_LIMIT = 1000

ANNOTATION_PREFIXES = (
    "ra8_test_helper",
    "ra8_internal",
    "ra8_priv",
    "ra8_di_slot",
    "ra8_nsc_veneer",
    # Must match exactly what RA8_HW_REGISTER_ACCESS emits in
    # libs/ra8_core/inc/ra8_attributes.h. It previously read "ra8_hw_mmio",
    # a string the macro never emitted, so rule 6 matched nothing and every
    # MMIO accessor went unchecked.
    "ra8_hw_register_access",
    "ra8_p10_rule3_exception",
    "ra8_mcdc_deactivated",
    "ra8_stack_max",
    "ra8_isr_safe",
    "ra8_expects_lock",
    "ra8_host_friendly",
    "ra8_latency_max_ns",
    "ra8_no_recursion",
    "ra8_bounded_loop",
    "ra8_validates",
    "ra8_owns_resource",
    "ra8_releases_resource",
    "ra8_reviewed_by",
    "ra8_register_bank",
    "ra8_isr_handler",
)


# --------------------------------------------------------------------------
# libclang import. The package is normally installed via:
#   python3 -m pip install --user --break-system-packages libclang
# In the dev container the wheel ships a bundled libclang.dylib / .so so
# no system-level libclang package is required.
# --------------------------------------------------------------------------
try:
    from clang import cindex
except ImportError:
    sys.stderr.write(
        "check_annotations.py: 'libclang' Python package missing.\n"
        "  install: python3 -m pip install --user --break-system-packages libclang\n"
    )
    sys.exit(0 if WARN_ONLY_MODE else 2)


# ``CursorKind.SECTION_ATTR`` is not exposed by every libclang release -- the
# 18.1.x wheels used on the runners omit it. Resolve it once and fall back to
# None so a missing kind degrades to "no section attribute detected" instead of
# raising AttributeError, which previously aborted the entire TU walk (silently
# dropping every TU that defined an annotated function in-place, e.g.
# ra8_widget_*, ra8_book, ra8_rabook_*).
SECTION_ATTR_KIND = getattr(cindex.CursorKind, "SECTION_ATTR", None)


# --------------------------------------------------------------------------
# Data model
# --------------------------------------------------------------------------
@dataclass
class AnnotatedSymbol:
    """A function definition (or declaration) carrying ra8_* annotations."""

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
    in_address_of: bool = False
    parent_loop_label: str | None = None  # for RA8_PROTECTED_WRITE detection
    preceding_comment: str = ""


@dataclass
class Violation:
    rule: str
    file: str
    line: int
    message: str
    warn_only: bool = False


# --------------------------------------------------------------------------
# AST walk
# --------------------------------------------------------------------------
def is_excluded(path: pathlib.Path) -> bool:
    return any(part in EXCLUDED_PATH_PARTS for part in path.parts)


def discover_translation_units() -> list[pathlib.Path]:
    """Return every .c/.cpp file under SCAN_DIRS, excluding vendored trees."""
    out: list[pathlib.Path] = []
    for top in SCAN_DIRS:
        root = REPO_ROOT / top
        if not root.is_dir():
            continue
        out.extend(
            path
            for path in root.rglob("*")
            if path.suffix in SOURCE_SUFFIXES and not is_excluded(path)
        )
    return sorted(out)


def collect_annotations(cursor: cindex.Cursor) -> list[str]:
    """Return every ra8_* string attached to a cursor via AnnotateAttr."""
    out: list[str] = []
    for child in cursor.get_children():
        if child.kind == cindex.CursorKind.ANNOTATE_ATTR:
            text = child.spelling or child.displayname or ""
            if text.startswith(ANNOTATION_PREFIXES):
                out.append(text)
    return out


def _include_args() -> list[str]:
    """Return -I flags covering every first-party header root.

    The include path must be complete, and it must be derived only from
    the repo layout. It used to list four hand-picked directories on the
    theory that unresolved headers still yield a good-enough AST. They do
    not: a call whose declaration was never seen does not resolve, so
    ``cursor.referenced`` is None and the call site is dropped. That
    silently starved the call-graph rules (``ra8_priv``,
    ``ra8_test_helper``) of exactly the cross-module call sites they
    exist to police -- the light path resolved 43k of 126k call sites,
    and which ones resolved varied with ambient state, so the same tree
    could pass standalone and fail under the pre-commit hook.
    """
    roots: list[pathlib.Path] = []
    for pattern in (
        "libs/*/inc",
        "libs/*/src",
        "src/inc",
        "src/secure_app",
        "tests/include",
        "tests",
        "port/*/inc",
    ):
        roots.extend(sorted(REPO_ROOT.glob(pattern)))
    # libs/<mod>/src holds the *_internal.h headers that declare RA8_PRIV
    # symbols; without them those declarations never resolve.
    return [f"-I{d}" for d in roots if d.is_dir() and "third_party" not in d.parts]


def _builtin_include_args() -> list[str]:
    """Return -isystem flags for clang's own builtin headers.

    libclang loaded through the Python bindings does not know where its
    resource directory lives, so ``stddef.h`` and friends may not resolve.
    That is not cosmetic: a failed system include aborts the rest of the
    include chain, later declarations are never seen, and the calls that
    depend on them silently vanish from the call graph. Ask a real clang
    binary where its resource dir is and put that on the path.
    """
    candidates = [os.environ.get("RA8_CLANG"), "clang", "clang-22", "clang-21", "clang-20"]
    for exe in candidates:
        if not exe:
            continue
        try:
            res = subprocess.run(  # noqa: S603  # fixed argv, no shell
                [exe, "-print-resource-dir"],
                capture_output=True,
                text=True,
                timeout=20,
                check=False,
            )
        except (OSError, subprocess.SubprocessError):
            continue
        if res.returncode != 0:
            continue
        inc = pathlib.Path(res.stdout.strip()) / "include"
        if not (inc / "stddef.h").is_file():
            continue
        # Only adopt this resource dir if it is actually compatible with the
        # loaded libclang. A mismatched pair (e.g. clang 22 headers against
        # older bindings) fails inside stdint.h with "__INT32_C is not
        # defined", which is worse than not setting it at all.
        if _probe_is_clean([f"-isystem{inc}"]):
            return [f"-isystem{inc}"]
    return []


def _probe_is_clean(extra: list[str]) -> bool:
    """True when ``#include <stddef.h>`` parses without a fatal error."""
    probe = REPO_ROOT / "scripts" / "utils" / ".ra8_annotations_probe.c"
    try:
        probe.write_text("#include <stddef.h>\n#include <stdint.h>\nsize_t ra8_probe(void);\n")
        tu = cindex.Index.create().parse(
            str(probe), args=["-std=c23", "-x", "c", "-DRA8_HOST_BUILD=1", *extra]
        )
        return not [d for d in (tu.diagnostics if tu else []) if d.severity >= _DIAG_ERROR]
    except cindex.TranslationUnitLoadError:
        return False
    finally:
        with contextlib.suppress(OSError):
            probe.unlink()


def _warn_if_builtins_missing(args: list[str]) -> None:
    """Report on stderr when clang's builtin headers do not resolve.

    This is a completeness warning, not a verdict. When ``stddef.h`` is
    unreachable the include chain aborts at the first system header, every
    declaration after it goes unseen, and the call sites that depend on
    those declarations disappear from the graph -- so the call-graph rules
    (``ra8_priv``, ``ra8_test_helper``) quietly stop policing anything and
    the gate still prints success. Surfacing it keeps that state visible
    in CI logs instead of being mistaken for a clean tree.

    It deliberately does not exit non-zero: the resolution rate depends on
    the host's clang/SDK layout, and failing the build on an environment
    quirk would block every push rather than fix the analysis.
    """
    probe = REPO_ROOT / "scripts" / "utils" / ".ra8_annotations_probe.c"
    try:
        probe.write_text("#include <stddef.h>\nsize_t ra8_probe(void);\n")
        tu = cindex.Index.create().parse(str(probe), args=args)
        bad = [
            d
            for d in (tu.diagnostics if tu else [])
            if d.severity >= _DIAG_ERROR and "file not found" in d.spelling
        ]
    except cindex.TranslationUnitLoadError:
        bad = []
    finally:
        with contextlib.suppress(OSError):
            probe.unlink()
    if bad:
        print(
            "check_annotations: WARNING -- clang builtin headers are not on "
            f"the include path ({bad[0].spelling}). The call graph will be "
            "incomplete, so the ra8_priv / ra8_test_helper caller rules may "
            "not fire. Set RA8_CLANG=<path-to-clang> to fix.",
            file=sys.stderr,
        )


#: Cached include flags -- the glob is stable for a whole run.
_INCLUDE_ARGS: list[str] = []


def parse_tu(path: pathlib.Path) -> cindex.TranslationUnit | None:
    """Parse a single TU using the project's general-purpose flags."""
    global _INCLUDE_ARGS  # noqa: PLW0603  # one-shot memo of a pure repo-layout scan
    if not _INCLUDE_ARGS:
        _INCLUDE_ARGS = _builtin_include_args() + _include_args()
        _warn_if_builtins_missing(["-std=c23", "-x", "c", "-DRA8_HOST_BUILD=1", *_INCLUDE_ARGS])
    args = [
        "-std=c23",
        "-x",
        "c",
        "-DRA8_HOST_BUILD=1",
        *_INCLUDE_ARGS,
    ]
    index = cindex.Index.create()
    try:
        tu = index.parse(
            str(path),
            args=args,
            options=cindex.TranslationUnit.PARSE_DETAILED_PROCESSING_RECORD,
        )
    except cindex.TranslationUnitLoadError:
        return None
    return tu


def walk_tu(
    tu: cindex.TranslationUnit,
    _tu_path: pathlib.Path,
    symbols: dict[str, AnnotatedSymbol],
    calls: list[CallSite],
) -> None:
    """Single-pass AST walk that fills ``symbols`` and ``calls``."""

    def file_of(c: cindex.Cursor) -> str:
        if c.location.file is None:
            return ""
        return str(c.location.file)

    def visit(node: cindex.Cursor, current_func: cindex.Cursor | None) -> None:  # noqa: PLR0912  # AST dispatch, splitting hurts readability
        # Function declarations / definitions
        if node.kind == cindex.CursorKind.FUNCTION_DECL:
            anns = collect_annotations(node)
            if anns:
                # Promote / merge into the symbol table
                fname = node.spelling
                sym = symbols.setdefault(
                    fname,
                    AnnotatedSymbol(name=fname, file=file_of(node), line=node.location.line),
                )
                for a in anns:
                    if a not in sym.annotations:
                        sym.annotations.append(a)
                sym.is_static = node.linkage == cindex.LinkageKind.INTERNAL or sym.is_static
                sym.return_type = node.result_type.spelling
                if any(arg.type.kind == cindex.TypeKind.POINTER for arg in node.get_arguments()):
                    sym.has_pointer_param = True
                if not sym.usr:
                    with contextlib.suppress(Exception):
                        sym.usr = node.canonical.get_usr()
                if node.is_definition():
                    sym.file = file_of(node)
                    sym.line = node.location.line
                    sym.end_line = node.extent.end.line
                # libclang exposes inline-ness via the cursor's tokens
                tokens = [t.spelling for t in node.get_tokens()]
                if "inline" in tokens or "__inline__" in tokens:
                    sym.has_inline = True
                # Section attribute (only when this libclang exposes the kind).
                if SECTION_ATTR_KIND is not None:
                    for child in node.get_children():
                        if child.kind == SECTION_ATTR_KIND:
                            sym.section = child.spelling
            # Recurse into the function body with new ``current_func``.
            for child in node.get_children():
                visit(child, node if node.is_definition() else current_func)
            return

        # Direct calls inside a function body
        if node.kind == cindex.CursorKind.CALL_EXPR and current_func is not None:
            callee = node.referenced
            if callee is not None:
                callee_usr = ""
                with contextlib.suppress(Exception):
                    callee_usr = callee.canonical.get_usr()
                cs = CallSite(
                    callee_name=callee.spelling,
                    caller_name=current_func.spelling,
                    caller_file=file_of(node),
                    caller_line=node.location.line,
                    callee_usr=callee_usr,
                )
                calls.append(cs)

        # Address-of: &foo
        if node.kind == cindex.CursorKind.UNARY_OPERATOR:
            tokens = [t.spelling for t in node.get_tokens()]
            if tokens and tokens[0] == "&":
                calls.extend(
                    CallSite(
                        callee_name=child.referenced.spelling,
                        caller_name=(current_func.spelling if current_func else ""),
                        caller_file=file_of(node),
                        caller_line=node.location.line,
                        in_address_of=True,
                    )
                    for child in node.get_children()
                    if (
                        child.kind == cindex.CursorKind.DECL_REF_EXPR
                        and child.referenced
                        and child.referenced.kind == cindex.CursorKind.FUNCTION_DECL
                    )
                )

        for child in node.get_children():
            visit(child, current_func)

    visit(tu.cursor, None)


# --------------------------------------------------------------------------
# Rule helpers
# --------------------------------------------------------------------------
#: libclang diagnostic severity for Error (3) and Fatal (4); anything at or
#: above this level means the parse did not see the code it was given.
_DIAG_ERROR = 3

#: The two ways a veneer legitimately validates an NS address range.
_NSC_RANGE_CHECK_RE = re.compile(r"RA8_NSC_CHECK_NS_RANGE_(?:R|RW)\b|cmse_check_address_range\b")

#: Cache of source files read back for textual (macro-level) checks.
_SOURCE_CACHE: dict[str, list[str]] = {}


def _source_lines(path: str) -> list[str]:
    """Return ``path``'s lines, cached. Empty list when unreadable."""
    if path not in _SOURCE_CACHE:
        try:
            _SOURCE_CACHE[path] = pathlib.Path(path).read_text(errors="ignore").splitlines()
        except OSError:
            _SOURCE_CACHE[path] = []
    return _SOURCE_CACHE[path]


def _definition_text(sym: AnnotatedSymbol) -> str:
    """Return the source text of ``sym``'s definition.

    Used for checks that must see the code *before* preprocessing, because
    the construct being looked for is a macro that this script's parse
    configuration expands away (see the NSC range-check rule).
    """
    lines = _source_lines(sym.file)
    if not lines or sym.line <= 0:
        return ""
    end = sym.end_line if sym.end_line >= sym.line else len(lines)
    return "\n".join(lines[sym.line - 1 : end])


def parse_annotation(ann: str) -> tuple[str, str]:
    """Split ``ra8_stack_max:512`` -> (``ra8_stack_max``, ``512``)."""
    if ":" in ann:
        rule, _, arg = ann.partition(":")
        return rule.strip(), arg.strip()
    return ann.strip(), ""


def _same_symbol(cs: CallSite, sym: AnnotatedSymbol) -> bool:
    """True when ``cs`` really calls ``sym`` and not a namesake.

    C lets every translation unit have its own ``static`` helper with the
    same name, and this repo does exactly that (three unrelated
    ``internal_zero_bytes`` live in net_pal, usb_hmsc and usb_pmsc). The
    symbol table is keyed by name, so without this check an annotation on
    one of them is attributed to calls of all of them -- reporting a
    cross-module violation against a module that only ever called its own
    file-local copy. USRs encode the defining file for internal-linkage
    symbols, so they separate the namesakes exactly.

    Falls back to the name match when either USR is unavailable, so a
    libclang build that cannot produce USRs degrades to the old behaviour
    rather than silently skipping every rule.
    """
    if not cs.callee_usr or not sym.usr:
        return True
    return cs.callee_usr == sym.usr


def _is_test_path(path: str) -> bool:
    """True when ``path`` is a host unit-test translation unit."""
    return "/tests/" in path.replace("\\", "/")


def module_of(path: str) -> str | None:
    """libs/<module>/... -> '<module>'; else None."""
    p = pathlib.Path(path)
    try:
        idx = p.parts.index("libs")
    except ValueError:
        return None
    if idx + 1 < len(p.parts):
        return p.parts[idx + 1]
    return None


def find_su_file(symbol: AnnotatedSymbol) -> int | None:
    """Look for a matching .su entry under any examples/*/build*/ tree."""
    name = symbol.name
    examples = REPO_ROOT / "examples"
    if not examples.is_dir():
        return None
    pat = re.compile(r"\b" + re.escape(name) + r"\b\s+(\d+)\s+\w+")
    for su in examples.rglob("*.su"):
        with contextlib.suppress(OSError):
            for line in su.read_text(errors="ignore").splitlines():
                m = pat.search(line)
                if m:
                    return int(m.group(1))
    return None


# --------------------------------------------------------------------------
# Rule enforcement
# --------------------------------------------------------------------------
def enforce_rules(  # noqa: PLR0912 PLR0915  # rule-dispatch table; splitting by rule reduces clarity
    symbols: dict[str, AnnotatedSymbol],
    calls: list[CallSite],
) -> list[Violation]:
    out: list[Violation] = []

    # Build per-callee call list and per-caller call list for fast lookup.
    direct_calls_by_callee: dict[str, list[CallSite]] = {}
    direct_calls_by_caller: dict[str, list[CallSite]] = {}
    for cs in calls:
        if not cs.in_address_of:
            direct_calls_by_callee.setdefault(cs.callee_name, []).append(cs)
        direct_calls_by_caller.setdefault(cs.caller_name, []).append(cs)

    address_taken: set[str] = {cs.callee_name for cs in calls if cs.in_address_of}

    for sym in symbols.values():
        for ann in sym.annotations:
            rule, arg = parse_annotation(ann)

            warn_only = rule in WARN_ONLY_RULES

            # 1. ra8_test_helper -- callers must live under /tests/
            if rule == "ra8_test_helper":
                out.extend(
                    Violation(
                        rule,
                        cs.caller_file,
                        cs.caller_line,
                        f"function '{sym.name}' tagged RA8_TEST_HELPER "
                        "called from non-test context",
                    )
                    for cs in direct_calls_by_callee.get(sym.name, [])
                    if _same_symbol(cs, sym) and not _is_test_path(cs.caller_file)
                )

            # 2. ra8_internal -- definition must be static
            elif rule == "ra8_internal":
                if not sym.is_static:
                    out.append(
                        Violation(
                            rule,
                            sym.file,
                            sym.line,
                            f"function '{sym.name}' tagged RA8_INTERNAL is not declared static",
                        )
                    )

            # 3. ra8_priv -- callers must share the same libs/<module>/
            elif rule == "ra8_priv":
                callee_mod = module_of(sym.file)
                if callee_mod is None:
                    continue
                for cs in direct_calls_by_callee.get(sym.name, []):
                    if not _same_symbol(cs, sym):
                        continue
                    caller_mod = module_of(cs.caller_file)
                    # Host unit tests are a sanctioned consumer of a module's
                    # promoted internals: CLAUDE.md ("Test access to internal
                    # symbols") allows a helper to drop `static` and be
                    # declared in the module's _internal.h precisely so tests
                    # can reach the validation paths for MC/DC. Production
                    # callers in *other* modules remain violations.
                    if _is_test_path(cs.caller_file):
                        continue
                    if caller_mod != callee_mod:
                        out.append(
                            Violation(
                                rule,
                                cs.caller_file,
                                cs.caller_line,
                                f"function '{sym.name}' tagged RA8_PRIV "
                                f"({callee_mod}) called from outside its module "
                                f"(caller={caller_mod or 'unknown'})",
                            )
                        )

            # 4. ra8_di_slot:<role> -- must be referenced via &name, not direct
            elif rule == "ra8_di_slot":
                if sym.name in direct_calls_by_callee and sym.name not in address_taken:
                    cs = direct_calls_by_callee[sym.name][0]
                    out.append(
                        Violation(
                            rule,
                            cs.caller_file,
                            cs.caller_line,
                            f"function '{sym.name}' tagged RA8_DI_SLOT "
                            f"called directly; must be invoked via function "
                            f"pointer (DIP)",
                            warn_only=True,  # heuristic; warn until pattern stable
                        )
                    )

            # 5. ra8_nsc_veneer
            elif rule == "ra8_nsc_veneer":
                f = sym.file.replace("\\", "/")
                if "/libs/ra8_nsc/src/" not in f:
                    out.append(
                        Violation(
                            rule,
                            sym.file,
                            sym.line,
                            f"NSC veneer '{sym.name}' must live under "
                            f"libs/ra8_nsc/src/ (found {f})",
                        )
                    )
                # A veneer must range-check every address it accepts from the
                # Non-Secure world. Veneers that take no pointer parameter
                # cross no address and need no check.
                #
                # The check is a textual scan, not a call-graph lookup: the
                # idiom is the RA8_NSC_CHECK_NS_RANGE_R/_RW macro, which
                # expands to cmse_check_address_range() only under -mcmse.
                # This script parses without -mcmse, so the macro expands to
                # a ((void)(p),(void)(n)) no-op and leaves no CallExpr for
                # libclang to find. The previous call-graph form looked for a
                # callee named "ra8_nsc_check_*" -- a spelling no veneer has
                # ever used -- so it could only ever produce false positives.
                if sym.has_pointer_param:
                    body = _definition_text(sym)
                    if not _NSC_RANGE_CHECK_RE.search(body):
                        out.append(
                            Violation(
                                rule,
                                sym.file,
                                sym.line,
                                f"NSC veneer '{sym.name}' takes a pointer from NS but "
                                f"never range-checks it (expected "
                                f"RA8_NSC_CHECK_NS_RANGE_R/_RW or "
                                f"cmse_check_address_range)",
                            )
                        )
                if (
                    sym.section
                    and ".gnu.sgstubs" not in sym.section
                    and "cmse_nonsecure_entry" not in " ".join(sym.annotations)
                ):
                    out.append(
                        Violation(
                            rule,
                            sym.file,
                            sym.line,
                            f"NSC veneer '{sym.name}' must live in .gnu.sgstubs "
                            f"section or be cmse_nonsecure_entry",
                        )
                    )

            # 6. ra8_hw_register_access
            elif rule == "ra8_hw_register_access":
                if not sym.has_inline:
                    out.append(
                        Violation(
                            rule,
                            sym.file,
                            sym.line,
                            f"MMIO accessor '{sym.name}' must be inline",
                        )
                    )
                if "volatile" not in sym.return_type:
                    out.append(
                        Violation(
                            rule,
                            sym.file,
                            sym.line,
                            f"MMIO accessor '{sym.name}' must return volatile* "
                            f"(got '{sym.return_type}')",
                        )
                    )
                # Caller-side RA8_PROTECTED_WRITE / // CITES-OK: check is
                # left to a future textual scan; libclang loses macro
                # context that early.

            # 7. ra8_p10_rule3_exception -- malloc only inside this function
            elif rule == "ra8_p10_rule3_exception":
                # Nothing to check at the symbol; the global sweep below
                # ensures malloc/free/calloc/realloc only appear inside
                # tagged functions.
                pass

            # 8. ra8_mcdc_deactivated:<reason>
            elif rule == "ra8_mcdc_deactivated":
                if re.search(r"\.[ch]:\d+", arg):
                    out.append(
                        Violation(
                            rule,
                            sym.file,
                            sym.line,
                            f"ra8_mcdc_deactivated reason on '{sym.name}' "
                            f"contains file:line citation -- use function name",
                        )
                    )

            # 9. ra8_stack_max:<bytes>
            elif rule == "ra8_stack_max":
                try:
                    annotated = int(arg)
                except ValueError:
                    out.append(
                        Violation(
                            rule,
                            sym.file,
                            sym.line,
                            f"ra8_stack_max:'{arg}' on '{sym.name}' is not an integer byte count",
                        )
                    )
                    continue
                measured = find_su_file(sym)
                if measured is not None and measured > annotated:
                    out.append(
                        Violation(
                            rule,
                            sym.file,
                            sym.line,
                            f"function '{sym.name}' frame {measured} B exceeds "
                            f"ra8_stack_max:{annotated}",
                        )
                    )

            # 10. ra8_isr_safe -- handled by global ISR-chain pass below.
            elif rule == "ra8_isr_safe":
                pass

            # 11. ra8_expects_lock:<name>
            elif rule == "ra8_expects_lock":
                lock = arg
                for cs in direct_calls_by_callee.get(sym.name, []):
                    body = direct_calls_by_caller.get(cs.caller_name, [])
                    has_take = any(
                        c.callee_name == "RA8_TAKE_LOCK" and c.caller_line < cs.caller_line
                        for c in body
                    )
                    if not has_take:
                        out.append(
                            Violation(
                                rule,
                                cs.caller_file,
                                cs.caller_line,
                                f"call to '{sym.name}' (expects lock '{lock}') "
                                f'missing preceding RA8_TAKE_LOCK("{lock}")',
                            )
                        )

            # 12. ra8_host_friendly
            elif rule == "ra8_host_friendly":
                for cs in direct_calls_by_caller.get(sym.name, []):
                    callee_sym = symbols.get(cs.callee_name)
                    if callee_sym and any(
                        a.startswith(("ra8_hw_register_access", "RA8_HW_REGISTER_ACCESS"))
                        for a in callee_sym.annotations
                    ):
                        out.append(
                            Violation(
                                rule,
                                cs.caller_file,
                                cs.caller_line,
                                f"host-friendly '{sym.name}' calls MMIO "
                                f"accessor '{cs.callee_name}' (would break in sim)",
                            )
                        )

            # 13. ra8_latency_max_ns -- warn-only TODO until WCET pass exists.
            elif rule == "ra8_latency_max_ns":
                out.append(
                    Violation(
                        rule,
                        sym.file,
                        sym.line,
                        f"ra8_latency_max_ns:{arg} on '{sym.name}' deferred until WCET pass exists",
                        warn_only=True,
                    )
                )

            # 14. ra8_no_recursion -- transitive call closure must not include self
            elif rule == "ra8_no_recursion":
                seen = {sym.name}
                stack = [sym.name]
                recursive = False
                guard = 0
                while stack and guard < RECURSION_GUARD_LIMIT:
                    guard += 1
                    cur = stack.pop()
                    for cs in direct_calls_by_caller.get(cur, []):
                        if cs.in_address_of:
                            continue
                        if cs.callee_name == sym.name:
                            recursive = True
                            break
                        if cs.callee_name not in seen:
                            seen.add(cs.callee_name)
                            stack.append(cs.callee_name)
                    if recursive:
                        break
                if recursive:
                    out.append(
                        Violation(
                            rule,
                            sym.file,
                            sym.line,
                            f"function '{sym.name}' tagged RA8_NO_RECURSION "
                            f"appears in its own transitive call closure",
                        )
                    )

            # 15. ra8_bounded_loop:<symbol> -- textual fallback (libclang
            # loses for/while bounds easily; the textual pass is enough
            # to catch the common case).
            elif rule == "ra8_bounded_loop":
                try:
                    src = pathlib.Path(sym.file).read_text(errors="ignore")
                except OSError:
                    continue
                # crude function-body slice
                m = re.search(
                    re.escape(sym.name) + r"\s*\([^;]*\)\s*\{",
                    src,
                )
                if not m:
                    continue
                # capture braces until balanced
                start = m.end() - 1
                depth = 0
                end = start
                for i in range(start, len(src)):
                    if src[i] == "{":
                        depth += 1
                    elif src[i] == "}":
                        depth -= 1
                        if depth == 0:
                            end = i
                            break
                body = src[start:end]
                for loop_match in re.finditer(r"\b(for|while)\s*\(([^)]*)\)", body):
                    cond = loop_match.group(2)
                    if arg not in cond:
                        out.append(
                            Violation(
                                rule,
                                sym.file,
                                sym.line,
                                f"loop in '{sym.name}' missing bound symbol "
                                f"'{arg}' in condition '{cond.strip()}'",
                            )
                        )

            # 16. ra8_validates:<n>
            elif rule == "ra8_validates":
                try:
                    need = int(arg)
                except ValueError:
                    continue
                body = direct_calls_by_caller.get(sym.name, [])
                count = sum(1 for c in body if c.callee_name.startswith("RA8_CHECK_"))
                if count < need:
                    out.append(
                        Violation(
                            rule,
                            sym.file,
                            sym.line,
                            f"function '{sym.name}' has {count} RA8_CHECK_* calls; "
                            f"ra8_validates:{need} requires at least {need}",
                        )
                    )

            # 17. ra8_owns_resource:<kind> -- approximate: require at least
            # one matching ra8_releases_resource:<kind> call somewhere in body.
            elif rule == "ra8_owns_resource":
                kind = arg
                body = direct_calls_by_caller.get(sym.name, [])
                released = False
                for c in body:
                    callee = symbols.get(c.callee_name)
                    if callee and any(
                        a == f"ra8_releases_resource:{kind}" for a in callee.annotations
                    ):
                        released = True
                        break
                if not released:
                    out.append(
                        Violation(
                            rule,
                            sym.file,
                            sym.line,
                            f"function '{sym.name}' acquires '{kind}' but no "
                            f"matching ra8_releases_resource:{kind} call found",
                        )
                    )

            # 18. ra8_reviewed_by -- informational rollup only.
            elif rule == "ra8_reviewed_by":
                out.append(
                    Violation(
                        rule,
                        sym.file,
                        sym.line,
                        f"reviewed-by '{arg}' recorded for '{sym.name}'",
                        warn_only=True,
                    )
                )

            # 19. ra8_register_bank -- informational only.
            elif rule == "ra8_register_bank":
                out.append(
                    Violation(
                        rule,
                        sym.file,
                        sym.line,
                        f"register-bank '{arg}' recorded for '{sym.name}'",
                        warn_only=True,
                    )
                )

            if warn_only:
                # Mark synthesised entries as warn-only (idempotent).
                for v in out[-1:]:
                    v.warn_only = True

    # ----- Rule 7 global sweep: malloc/free/calloc/realloc usage --------
    p10_exempt = {
        sym.name
        for sym in symbols.values()
        if any(a.startswith("ra8_p10_rule3_exception") for a in sym.annotations)
    }
    bad_alloc = {"malloc", "free", "calloc", "realloc", "aligned_alloc"}
    for cs in calls:
        if cs.in_address_of:
            continue
        # Host-side test scaffolding is exempt from NASA P10 Rule 3 -- the
        # firmware itself never sees these TUs (see CLAUDE.md "Exempt Code").
        if "/tests/" in cs.caller_file.replace("\\", "/"):
            continue
        if cs.callee_name in bad_alloc and cs.caller_name not in p10_exempt:
            out.append(
                Violation(
                    "ra8_p10_rule3_exception",
                    cs.caller_file,
                    cs.caller_line,
                    f"call to '{cs.callee_name}' from '{cs.caller_name}' which "
                    f"is not tagged RA8_P10_RULE3_EXCEPTION",
                )
            )

    # ----- Rule 10 global sweep: ISR chain reachability -----------------
    isr_safe_names = {
        s.name for s in symbols.values() if any(a == "ra8_isr_safe" for a in s.annotations)
    }
    isr_handler_names = {
        s.name for s in symbols.values() if any(a == "ra8_isr_handler" for a in s.annotations)
    }
    if isr_handler_names or isr_safe_names:
        for handler in isr_handler_names:
            seen: set[str] = set()
            stack = [handler]
            while stack:
                cur = stack.pop()
                if cur in seen:
                    continue
                seen.add(cur)
                for cs in direct_calls_by_caller.get(cur, []):
                    if cs.in_address_of:
                        continue
                    callee = cs.callee_name
                    callee_sym = symbols.get(callee)
                    if callee_sym is None:
                        # External / libc -- treat as unsafe.
                        out.append(
                            Violation(
                                "ra8_isr_safe",
                                cs.caller_file,
                                cs.caller_line,
                                f"ISR handler '{handler}' transitively calls untagged '{callee}'",
                            )
                        )
                        continue
                    if not any(a == "ra8_isr_safe" for a in callee_sym.annotations):
                        out.append(
                            Violation(
                                "ra8_isr_safe",
                                cs.caller_file,
                                cs.caller_line,
                                f"ISR handler '{handler}' transitively calls "
                                f"'{callee}' which lacks RA8_ISR_SAFE",
                            )
                        )
                    stack.append(callee)

    return out


# --------------------------------------------------------------------------
# Entry point
# --------------------------------------------------------------------------
def main(argv: list[str]) -> int:  # noqa: PLR0912  # gate/parser dispatch, splitting hurts readability
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--check", action="store_true", help="exit non-zero on any (non-warn-only) violation"
    )
    ap.add_argument("--list", action="store_true", help="dump every annotated symbol and exit")
    ap.add_argument("--quiet", action="store_true", help="suppress per-TU progress output")
    ap.add_argument("paths", nargs="*", help="optional explicit file list (else scan everything)")
    args = ap.parse_args(argv)

    if args.paths:
        tus = [
            pathlib.Path(p).resolve()
            for p in args.paths
            if pathlib.Path(p).suffix in SOURCE_SUFFIXES
        ]
    else:
        tus = discover_translation_units()

    symbols: dict[str, AnnotatedSymbol] = {}
    calls: list[CallSite] = []

    for tu_path in tus:
        if not args.quiet and not args.check:
            print(f"  parsing {tu_path.relative_to(REPO_ROOT)}", file=sys.stderr)
        tu = parse_tu(tu_path)
        if tu is None:
            continue
        try:
            walk_tu(tu, tu_path, symbols, calls)
        except Exception as exc:
            sys.stderr.write(f"  WARN: walk failed for {tu_path}: {exc}\n")

    if args.list:
        if not symbols:
            print("(no annotated symbols found)")
            return 0
        print(f"{'symbol':<48} {'file':<60} {'annotations'}")
        for s in sorted(symbols.values(), key=lambda x: (x.file, x.line)):
            rel = pathlib.Path(s.file).resolve()
            with contextlib.suppress(ValueError):
                rel = rel.relative_to(REPO_ROOT)
            print(f"{s.name:<48} {rel!s:<60} {','.join(s.annotations)}")
        return 0

    violations = enforce_rules(symbols, calls)

    if not violations:
        if not args.quiet:
            print(
                f"check_annotations: 0 violations across "
                f"{len(symbols)} annotated symbols, "
                f"{len(calls)} call sites, {len(tus)} TUs"
            )
        return 0

    fatal = [v for v in violations if not v.warn_only]
    informational = [v for v in violations if v.warn_only]

    for v in violations:
        tag = "WARN" if v.warn_only else "FAIL"
        try:
            rel = pathlib.Path(v.file).resolve().relative_to(REPO_ROOT)
        except ValueError:
            rel = pathlib.Path(v.file)
        sys.stderr.write(f"[{tag}] {rel}:{v.line}: [{v.rule}] {v.message}\n")

    sys.stderr.write(f"check_annotations: {len(fatal)} fatal, {len(informational)} informational\n")

    if WARN_ONLY_MODE and not args.check:
        # mode: never fatal at the gate; promotion happens later.
        return 0
    return 1 if fatal else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
