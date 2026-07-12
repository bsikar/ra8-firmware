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
import pathlib
import re
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
    "ra8_hw_mmio",
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


@dataclass
class CallSite:
    """A direct CallExpr discovered during AST traversal."""

    callee_name: str
    caller_name: str
    caller_file: str
    caller_line: int
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


def parse_tu(path: pathlib.Path) -> cindex.TranslationUnit | None:
    """Parse a single TU using the project's general-purpose flags.

    We deliberately keep the include path light -- libclang will emit
    diagnostics for unresolved headers but the AST it produces is
    usually still good enough to spot ANNOTATE_ATTR markers and
    CallExpr cursors. Strict validation of cross-TU include hygiene
    is the job of clang-tidy, not this script.
    """
    args = [
        "-std=c23",
        "-x",
        "c",
        "-DRA8_HOST_BUILD=1",
        f"-I{REPO_ROOT}/libs/ra8_core/inc",
        f"-I{REPO_ROOT}/libs/ra8_hal/inc",
        f"-I{REPO_ROOT}/libs/ra8_nsc/inc",
        f"-I{REPO_ROOT}/src/inc",
        f"-I{REPO_ROOT}/tests/include",
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
                cs = CallSite(
                    callee_name=callee.spelling,
                    caller_name=current_func.spelling,
                    caller_file=file_of(node),
                    caller_line=node.location.line,
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
def parse_annotation(ann: str) -> tuple[str, str]:
    """Split ``ra8_stack_max:512`` -> (``ra8_stack_max``, ``512``)."""
    if ":" in ann:
        rule, _, arg = ann.partition(":")
        return rule.strip(), arg.strip()
    return ann.strip(), ""


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
                        f"function '{sym.name}' tagged RA8_TEST_HELPER called from non-test context",
                    )
                    for cs in direct_calls_by_callee.get(sym.name, [])
                    if "/tests/" not in cs.caller_file.replace("\\", "/")
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
                    caller_mod = module_of(cs.caller_file)
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
                            f"NSC veneer '{sym.name}' must live under libs/ra8_nsc/src/ (found {f})",
                        )
                    )
                body_calls = direct_calls_by_caller.get(sym.name, [])
                if not any(c.callee_name.startswith("ra8_nsc_check_") for c in body_calls):
                    out.append(
                        Violation(
                            rule,
                            sym.file,
                            sym.line,
                            f"NSC veneer '{sym.name}' missing ra8_nsc_check_* call in body",
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

            # 6. ra8_hw_mmio
            elif rule == "ra8_hw_mmio":
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
                        a.startswith(("ra8_hw_mmio", "RA8_HW_REGISTER_ACCESS"))
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
