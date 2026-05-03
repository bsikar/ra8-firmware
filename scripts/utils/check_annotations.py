#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""
check_annotations.py -- libclang-based annotation enforcement.

This script walks the AST of every C / C++ translation unit under
``libs/``, ``src/``, ``examples/``, ``tests/``, and ``port/`` and
applies the project's annotation-enforcement rules. The annotation
macros themselves are defined in ``libs/ra_core/inc/ra_attributes.h``
(produced by Wave 43a) and lower to GCC ``__attribute__((annotate
("ra_<rule>:...")))`` markers that libclang exposes via
``AnnotateAttr`` cursors.

The 19 enforceable rules are documented in ``docs/ANNOTATIONS.md``;
this script is the canonical implementation of their static checks.

For Wave 0 the script defaults to **warn-only** (mirrors the
``cite_check`` / ``check_world_tags`` pattern). Flip the
``WAVE_0_WARN_ONLY`` flag at the top of the file to ``False`` once
the codebase clears the gate.

Usage::

    python3 scripts/utils/check_annotations.py            # warn-only
    python3 scripts/utils/check_annotations.py --check    # CI gate
    python3 scripts/utils/check_annotations.py --list     # dump symbols
"""

from __future__ import annotations

import argparse
import os
import pathlib
import re
import sys
from dataclasses import dataclass, field
from typing import Iterable

# --------------------------------------------------------------------------
# Wave-0 warn-only gate. Flip to False to make violations fatal by default.
# --------------------------------------------------------------------------
WAVE_0_WARN_ONLY = True


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]

SCAN_DIRS = ("libs", "src", "examples", "tests", "port")
EXCLUDED_PATH_PARTS = {"build", "_deps", "third_party", "build-cov",
                       "build-bench", "build-scan", "build-mcdc"}
SOURCE_SUFFIXES = {".c", ".cpp"}

WARN_ONLY_RULES = {"ra_latency_max_ns", "ra_reviewed_by", "ra_register_bank"}

ANNOTATION_PREFIXES = (
    "ra_test_helper", "ra_internal", "ra_priv", "ra_di_slot",
    "ra_nsc_veneer", "ra_hw_mmio", "ra_p10_rule3_exception",
    "ra_mcdc_deactivated", "ra_stack_max", "ra_isr_safe",
    "ra_expects_lock", "ra_host_friendly", "ra_latency_max_ns",
    "ra_no_recursion", "ra_bounded_loop", "ra_validates",
    "ra_owns_resource", "ra_releases_resource", "ra_reviewed_by",
    "ra_register_bank", "ra_isr_handler",
)


# --------------------------------------------------------------------------
# libclang import. The package is normally installed via:
#   python3 -m pip install --user --break-system-packages libclang
# In the dev container the wheel ships a bundled libclang.dylib / .so so
# no system-level libclang package is required.
# --------------------------------------------------------------------------
try:
    import clang.cindex as cindex
except ImportError:
    sys.stderr.write(
        "check_annotations.py: 'libclang' Python package missing.\n"
        "  install: python3 -m pip install --user --break-system-packages libclang\n"
    )
    sys.exit(0 if WAVE_0_WARN_ONLY else 2)


# --------------------------------------------------------------------------
# Data model
# --------------------------------------------------------------------------
@dataclass
class AnnotatedSymbol:
    """A function definition (or declaration) carrying ra_* annotations."""

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
    parent_loop_label: str | None = None  # for RA_PROTECTED_WRITE detection
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
        for path in root.rglob("*"):
            if path.suffix in SOURCE_SUFFIXES and not is_excluded(path):
                out.append(path)
    return sorted(out)


def collect_annotations(cursor: cindex.Cursor) -> list[str]:
    """Return every ra_* string attached to a cursor via AnnotateAttr."""
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
        "-x", "c",
        "-DRA_HOST_BUILD=1",
        f"-I{REPO_ROOT}/libs/ra_core/inc",
        f"-I{REPO_ROOT}/libs/ra_hal/inc",
        f"-I{REPO_ROOT}/libs/ra_nsc/inc",
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
    tu_path: pathlib.Path,
    symbols: dict[str, AnnotatedSymbol],
    calls: list[CallSite],
) -> None:
    """Single-pass AST walk that fills ``symbols`` and ``calls``."""

    def file_of(c: cindex.Cursor) -> str:
        if c.location.file is None:
            return ""
        return str(c.location.file)

    def visit(node: cindex.Cursor, current_func: cindex.Cursor | None) -> None:
        # Function declarations / definitions
        if node.kind in (cindex.CursorKind.FUNCTION_DECL,):
            anns = collect_annotations(node)
            if anns:
                # Promote / merge into the symbol table
                fname = node.spelling
                sym = symbols.setdefault(
                    fname,
                    AnnotatedSymbol(name=fname, file=file_of(node),
                                    line=node.location.line),
                )
                for a in anns:
                    if a not in sym.annotations:
                        sym.annotations.append(a)
                sym.is_static = (
                    node.linkage == cindex.LinkageKind.INTERNAL
                    or sym.is_static
                )
                sym.return_type = node.result_type.spelling
                # libclang exposes inline-ness via the cursor's tokens
                tokens = [t.spelling for t in node.get_tokens()]
                if "inline" in tokens or "__inline__" in tokens:
                    sym.has_inline = True
                # Section attribute
                for child in node.get_children():
                    if child.kind == cindex.CursorKind.SECTION_ATTR:
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
                for child in node.get_children():
                    if child.kind == cindex.CursorKind.DECL_REF_EXPR and child.referenced:
                        if child.referenced.kind == cindex.CursorKind.FUNCTION_DECL:
                            # Mark address-of by recording a synthetic CallSite.
                            calls.append(CallSite(
                                callee_name=child.referenced.spelling,
                                caller_name=(current_func.spelling
                                             if current_func else ""),
                                caller_file=file_of(node),
                                caller_line=node.location.line,
                                in_address_of=True,
                            ))

        for child in node.get_children():
            visit(child, current_func)

    visit(tu.cursor, None)


# --------------------------------------------------------------------------
# Rule helpers
# --------------------------------------------------------------------------
def parse_annotation(ann: str) -> tuple[str, str]:
    """Split ``ra_stack_max:512`` -> (``ra_stack_max``, ``512``)."""
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
        try:
            for line in su.read_text(errors="ignore").splitlines():
                m = pat.search(line)
                if m:
                    return int(m.group(1))
        except OSError:
            continue
    return None


# --------------------------------------------------------------------------
# Rule enforcement
# --------------------------------------------------------------------------
def enforce_rules(
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

            # 1. ra_test_helper -- callers must live under /tests/
            if rule == "ra_test_helper":
                for cs in direct_calls_by_callee.get(sym.name, []):
                    if "/tests/" not in cs.caller_file.replace("\\", "/"):
                        out.append(Violation(
                            rule, cs.caller_file, cs.caller_line,
                            f"function '{sym.name}' tagged RA_TEST_HELPER "
                            f"called from non-test context",
                        ))

            # 2. ra_internal -- definition must be static
            elif rule == "ra_internal":
                if not sym.is_static:
                    out.append(Violation(
                        rule, sym.file, sym.line,
                        f"function '{sym.name}' tagged RA_INTERNAL is not "
                        f"declared static",
                    ))

            # 3. ra_priv -- callers must share the same libs/<module>/
            elif rule == "ra_priv":
                callee_mod = module_of(sym.file)
                if callee_mod is None:
                    continue
                for cs in direct_calls_by_callee.get(sym.name, []):
                    caller_mod = module_of(cs.caller_file)
                    if caller_mod != callee_mod:
                        out.append(Violation(
                            rule, cs.caller_file, cs.caller_line,
                            f"function '{sym.name}' tagged RA_PRIV "
                            f"({callee_mod}) called from outside its module "
                            f"(caller={caller_mod or 'unknown'})",
                        ))

            # 4. ra_di_slot:<role> -- must be referenced via &name, not direct
            elif rule == "ra_di_slot":
                if (sym.name in direct_calls_by_callee
                        and sym.name not in address_taken):
                    cs = direct_calls_by_callee[sym.name][0]
                    out.append(Violation(
                        rule, cs.caller_file, cs.caller_line,
                        f"function '{sym.name}' tagged RA_DI_SLOT "
                        f"called directly; must be invoked via function "
                        f"pointer (DIP)",
                        warn_only=True,  # heuristic; warn until pattern stable
                    ))

            # 5. ra_nsc_veneer
            elif rule == "ra_nsc_veneer":
                f = sym.file.replace("\\", "/")
                if "/libs/ra_nsc/src/" not in f:
                    out.append(Violation(
                        rule, sym.file, sym.line,
                        f"NSC veneer '{sym.name}' must live under "
                        f"libs/ra_nsc/src/ (found {f})",
                    ))
                body_calls = direct_calls_by_caller.get(sym.name, [])
                if not any(c.callee_name.startswith("ra_nsc_check_")
                           for c in body_calls):
                    out.append(Violation(
                        rule, sym.file, sym.line,
                        f"NSC veneer '{sym.name}' missing ra_nsc_check_* "
                        f"call in body",
                    ))
                if (sym.section
                        and ".gnu.sgstubs" not in sym.section
                        and "cmse_nonsecure_entry" not in " ".join(sym.annotations)):
                    out.append(Violation(
                        rule, sym.file, sym.line,
                        f"NSC veneer '{sym.name}' must live in .gnu.sgstubs "
                        f"section or be cmse_nonsecure_entry",
                    ))

            # 6. ra_hw_mmio
            elif rule == "ra_hw_mmio":
                if not sym.has_inline:
                    out.append(Violation(
                        rule, sym.file, sym.line,
                        f"MMIO accessor '{sym.name}' must be inline",
                    ))
                if "volatile" not in sym.return_type:
                    out.append(Violation(
                        rule, sym.file, sym.line,
                        f"MMIO accessor '{sym.name}' must return volatile* "
                        f"(got '{sym.return_type}')",
                    ))
                # Caller-side RA_PROTECTED_WRITE / // CITES-OK: check is
                # left to a future textual scan; libclang loses macro
                # context that early.

            # 7. ra_p10_rule3_exception -- malloc only inside this function
            elif rule == "ra_p10_rule3_exception":
                # Nothing to check at the symbol; the global sweep below
                # ensures malloc/free/calloc/realloc only appear inside
                # tagged functions.
                pass

            # 8. ra_mcdc_deactivated:<reason>
            elif rule == "ra_mcdc_deactivated":
                if re.search(r"\.[ch]:\d+", arg):
                    out.append(Violation(
                        rule, sym.file, sym.line,
                        f"ra_mcdc_deactivated reason on '{sym.name}' "
                        f"contains file:line citation -- use function name",
                    ))

            # 9. ra_stack_max:<bytes>
            elif rule == "ra_stack_max":
                try:
                    annotated = int(arg)
                except ValueError:
                    out.append(Violation(
                        rule, sym.file, sym.line,
                        f"ra_stack_max:'{arg}' on '{sym.name}' is not an "
                        f"integer byte count",
                    ))
                    continue
                measured = find_su_file(sym)
                if measured is not None and measured > annotated:
                    out.append(Violation(
                        rule, sym.file, sym.line,
                        f"function '{sym.name}' frame {measured} B exceeds "
                        f"ra_stack_max:{annotated}",
                    ))

            # 10. ra_isr_safe -- handled by global ISR-chain pass below.
            elif rule == "ra_isr_safe":
                pass

            # 11. ra_expects_lock:<name>
            elif rule == "ra_expects_lock":
                lock = arg
                for cs in direct_calls_by_callee.get(sym.name, []):
                    body = direct_calls_by_caller.get(cs.caller_name, [])
                    has_take = any(
                        c.callee_name == "RA_TAKE_LOCK"
                        and c.caller_line < cs.caller_line
                        for c in body
                    )
                    if not has_take:
                        out.append(Violation(
                            rule, cs.caller_file, cs.caller_line,
                            f"call to '{sym.name}' (expects lock '{lock}') "
                            f"missing preceding RA_TAKE_LOCK(\"{lock}\")",
                        ))

            # 12. ra_host_friendly
            elif rule == "ra_host_friendly":
                for cs in direct_calls_by_caller.get(sym.name, []):
                    callee_sym = symbols.get(cs.callee_name)
                    if callee_sym and any(
                        a.startswith("ra_hw_mmio") or
                        a.startswith("RA_HW_REGISTER_ACCESS")
                        for a in callee_sym.annotations
                    ):
                        out.append(Violation(
                            rule, cs.caller_file, cs.caller_line,
                            f"host-friendly '{sym.name}' calls MMIO "
                            f"accessor '{cs.callee_name}' (would break in sim)",
                        ))

            # 13. ra_latency_max_ns -- warn-only TODO until WCET pass exists.
            elif rule == "ra_latency_max_ns":
                out.append(Violation(
                    rule, sym.file, sym.line,
                    f"ra_latency_max_ns:{arg} on '{sym.name}' deferred "
                    f"until WCET pass exists",
                    warn_only=True,
                ))

            # 14. ra_no_recursion -- transitive call closure must not include self
            elif rule == "ra_no_recursion":
                seen = {sym.name}
                stack = [sym.name]
                recursive = False
                guard = 0
                while stack and guard < 1000:
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
                    out.append(Violation(
                        rule, sym.file, sym.line,
                        f"function '{sym.name}' tagged RA_NO_RECURSION "
                        f"appears in its own transitive call closure",
                    ))

            # 15. ra_bounded_loop:<symbol> -- textual fallback (libclang
            # loses for/while bounds easily; the textual pass is enough
            # to catch the common case).
            elif rule == "ra_bounded_loop":
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
                for loop_match in re.finditer(
                    r"\b(for|while)\s*\(([^)]*)\)", body
                ):
                    cond = loop_match.group(2)
                    if arg not in cond:
                        out.append(Violation(
                            rule, sym.file, sym.line,
                            f"loop in '{sym.name}' missing bound symbol "
                            f"'{arg}' in condition '{cond.strip()}'",
                        ))

            # 16. ra_validates:<n>
            elif rule == "ra_validates":
                try:
                    need = int(arg)
                except ValueError:
                    continue
                body = direct_calls_by_caller.get(sym.name, [])
                count = sum(
                    1 for c in body
                    if c.callee_name.startswith("RA_CHECK_")
                )
                if count < need:
                    out.append(Violation(
                        rule, sym.file, sym.line,
                        f"function '{sym.name}' has {count} RA_CHECK_* calls; "
                        f"ra_validates:{need} requires at least {need}",
                    ))

            # 17. ra_owns_resource:<kind> -- approximate: require at least
            # one matching ra_releases_resource:<kind> call somewhere in body.
            elif rule == "ra_owns_resource":
                kind = arg
                body = direct_calls_by_caller.get(sym.name, [])
                released = False
                for c in body:
                    callee = symbols.get(c.callee_name)
                    if callee and any(
                        a == f"ra_releases_resource:{kind}"
                        for a in callee.annotations
                    ):
                        released = True
                        break
                if not released:
                    out.append(Violation(
                        rule, sym.file, sym.line,
                        f"function '{sym.name}' acquires '{kind}' but no "
                        f"matching ra_releases_resource:{kind} call found",
                    ))

            # 18. ra_reviewed_by -- informational rollup only.
            elif rule == "ra_reviewed_by":
                out.append(Violation(
                    rule, sym.file, sym.line,
                    f"reviewed-by '{arg}' recorded for '{sym.name}'",
                    warn_only=True,
                ))

            # 19. ra_register_bank -- informational only.
            elif rule == "ra_register_bank":
                out.append(Violation(
                    rule, sym.file, sym.line,
                    f"register-bank '{arg}' recorded for '{sym.name}'",
                    warn_only=True,
                ))

            if warn_only:
                # Mark synthesised entries as warn-only (idempotent).
                for v in out[-1:]:
                    v.warn_only = True

    # ----- Rule 7 global sweep: malloc/free/calloc/realloc usage --------
    p10_exempt = {
        sym.name for sym in symbols.values()
        if any(a.startswith("ra_p10_rule3_exception")
               for a in sym.annotations)
    }
    BAD_ALLOC = {"malloc", "free", "calloc", "realloc", "aligned_alloc"}
    for cs in calls:
        if cs.in_address_of:
            continue
        # Host-side test scaffolding is exempt from NASA P10 Rule 3 -- the
        # firmware itself never sees these TUs (see CLAUDE.md "Exempt Code").
        if "/tests/" in cs.caller_file.replace("\\", "/"):
            continue
        if cs.callee_name in BAD_ALLOC and cs.caller_name not in p10_exempt:
            out.append(Violation(
                "ra_p10_rule3_exception", cs.caller_file, cs.caller_line,
                f"call to '{cs.callee_name}' from '{cs.caller_name}' which "
                f"is not tagged RA_P10_RULE3_EXCEPTION",
            ))

    # ----- Rule 10 global sweep: ISR chain reachability -----------------
    isr_safe_names = {
        s.name for s in symbols.values()
        if any(a == "ra_isr_safe" for a in s.annotations)
    }
    isr_handler_names = {
        s.name for s in symbols.values()
        if any(a == "ra_isr_handler" for a in s.annotations)
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
                        out.append(Violation(
                            "ra_isr_safe", cs.caller_file, cs.caller_line,
                            f"ISR handler '{handler}' transitively calls "
                            f"untagged '{callee}'",
                        ))
                        continue
                    if not any(a == "ra_isr_safe"
                               for a in callee_sym.annotations):
                        out.append(Violation(
                            "ra_isr_safe", cs.caller_file, cs.caller_line,
                            f"ISR handler '{handler}' transitively calls "
                            f"'{callee}' which lacks RA_ISR_SAFE",
                        ))
                    stack.append(callee)

    return out


# --------------------------------------------------------------------------
# Entry point
# --------------------------------------------------------------------------
def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true",
                    help="exit non-zero on any (non-warn-only) violation")
    ap.add_argument("--list", action="store_true",
                    help="dump every annotated symbol and exit")
    ap.add_argument("--quiet", action="store_true",
                    help="suppress per-TU progress output")
    ap.add_argument("paths", nargs="*",
                    help="optional explicit file list (else scan everything)")
    args = ap.parse_args(argv)

    if args.paths:
        tus = [pathlib.Path(p).resolve() for p in args.paths
               if pathlib.Path(p).suffix in SOURCE_SUFFIXES]
    else:
        tus = discover_translation_units()

    symbols: dict[str, AnnotatedSymbol] = {}
    calls: list[CallSite] = []

    for tu_path in tus:
        if not args.quiet and not args.check:
            print(f"  parsing {tu_path.relative_to(REPO_ROOT)}",
                  file=sys.stderr)
        tu = parse_tu(tu_path)
        if tu is None:
            continue
        try:
            walk_tu(tu, tu_path, symbols, calls)
        except Exception as exc:  # noqa: BLE001 -- best-effort tool
            sys.stderr.write(
                f"  WARN: walk failed for {tu_path}: {exc}\n"
            )

    if args.list:
        if not symbols:
            print("(no annotated symbols found)")
            return 0
        print(f"{'symbol':<48} {'file':<60} {'annotations'}")
        for s in sorted(symbols.values(), key=lambda x: (x.file, x.line)):
            rel = pathlib.Path(s.file).resolve()
            try:
                rel = rel.relative_to(REPO_ROOT)
            except ValueError:
                pass
            print(f"{s.name:<48} {str(rel):<60} {','.join(s.annotations)}")
        return 0

    violations = enforce_rules(symbols, calls)

    if not violations:
        if not args.quiet:
            print(f"check_annotations: 0 violations across "
                  f"{len(symbols)} annotated symbols, "
                  f"{len(calls)} call sites, {len(tus)} TUs")
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

    sys.stderr.write(
        f"check_annotations: {len(fatal)} fatal, "
        f"{len(informational)} informational\n"
    )

    if WAVE_0_WARN_ONLY and not args.check:
        # Wave-0 mode: never fatal at the gate; promotion happens later.
        return 0
    return 1 if fatal else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
