# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""One function per annotation rule, dispatched by rule key.

This was a single 450-line ``enforce_rules()`` carrying an ``elif`` chain of
nineteen rules and a ``noqa`` asserting that splitting it "reduces clarity".
It did the opposite: the chain shared one mutable ``out`` list, so a rule's
effect was only readable by tracking every append across the whole body -- and
the informational-rule marker at the bottom reached back into ``out[-1]``,
which silently belongs to whichever rule appended last rather than to the rule
being processed.

Each rule is now a function of ``(symbol, argument, context)`` returning its
own findings, and :data:`RULE_CHECKS` maps the key to the function.  Adding a
rule is adding an entry; a rule's whole behaviour is one screen.  The keys are
cross-checked against what ``ra8_attributes.h`` emits by
:func:`annot_rulekeys.check_rule_keys`, so an entry keyed on a spelling no
macro produces is a failure rather than a rule that quietly never matches.
"""

from __future__ import annotations

import contextlib
import pathlib
import re
from collections.abc import Callable
from dataclasses import dataclass

from annot_linkage import enforce_linkage
from annot_model import AnnotatedSymbol, CallSite, Violation
from annot_rulekeys import INFORMATIONAL_RULES, parse_annotation
from annot_scope import is_host_only_path, is_test_path, module_of, repo_root
from annot_source import definition_text

#: Upper bound on the call-graph BFS used for ra8_no_recursion checking.
#: Prevents infinite loops on pathological call graphs during static analysis.
RECURSION_GUARD_LIMIT = 1000

#: The two ways a veneer legitimately validates an NS address range.
_NSC_RANGE_CHECK_RE = re.compile(r"RA8_NSC_CHECK_NS_RANGE_(?:R|RW)\b|cmse_check_address_range\b")

#: Allocation entry points NASA P10 Rule 3 forbids in firmware.
_ALLOCATORS = frozenset({"malloc", "free", "calloc", "realloc", "aligned_alloc"})

#: Where an NSC veneer must be defined.
_NSC_SRC_DIR = "/libs/ra8_nsc/src/"


@dataclass
class RuleCtx:
    """Everything a rule needs beyond the symbol it is judging.

    Both call indexes are keyed by USR. A name key would merge every module's
    namesake ``static`` helper into one bucket and enforce one module's
    annotation against another module's callers -- the defect the symbol table
    itself was rekeyed to end.
    """

    symbols: dict[str, AnnotatedSymbol]
    calls_by_callee: dict[str, list[CallSite]]
    calls_by_caller: dict[str, list[CallSite]]
    address_taken: set[str]

    @classmethod
    def build(cls, symbols: dict[str, AnnotatedSymbol], calls: list[CallSite]) -> RuleCtx:
        """Index ``calls`` by callee and by caller for constant-time lookup."""
        by_callee: dict[str, list[CallSite]] = {}
        by_caller: dict[str, list[CallSite]] = {}
        for cs in calls:
            if not cs.in_address_of:
                by_callee.setdefault(cs.callee_usr, []).append(cs)
            by_caller.setdefault(cs.caller_usr, []).append(cs)
        return cls(
            symbols=symbols,
            calls_by_callee=by_callee,
            calls_by_caller=by_caller,
            address_taken={cs.callee_usr for cs in calls if cs.in_address_of},
        )

    def callers_of(self, sym: AnnotatedSymbol) -> list[CallSite]:
        """Every direct call site targeting ``sym``."""
        return self.calls_by_callee.get(sym.usr, [])

    def body_calls(self, sym: AnnotatedSymbol) -> list[CallSite]:
        """Every call ``sym``'s own body makes."""
        return self.calls_by_caller.get(sym.usr, [])


def find_su_file(symbol: AnnotatedSymbol) -> int | None:
    """Look for a matching .su entry under any examples/*/build*/ tree."""
    examples = repo_root() / "examples"
    if not examples.is_dir():
        return None
    pat = re.compile(r"\b" + re.escape(symbol.name) + r"\b\s+(\d+)\s+\w+")
    for su in examples.rglob("*.su"):
        with contextlib.suppress(OSError):
            for line in su.read_text(errors="ignore").splitlines():
                m = pat.search(line)
                if m:
                    return int(m.group(1))
    return None


def _at(sym: AnnotatedSymbol, rule: str, message: str) -> Violation:
    """A finding located at ``sym``'s own definition."""
    return Violation(rule, sym.file, sym.line, message)


def _at_call(cs: CallSite, rule: str, message: str) -> Violation:
    """A finding located at a call site rather than at the callee."""
    return Violation(rule, cs.caller_file, cs.caller_line, message)


# --------------------------------------------------------------------------
# Rule implementations. Signature: (symbol, annotation argument, context).
# --------------------------------------------------------------------------
def _rule_test_helper(sym: AnnotatedSymbol, _arg: str, ctx: RuleCtx) -> list[Violation]:
    """RA8_TEST_HELPER: every caller must live under tests/."""
    return [
        _at_call(
            cs,
            "ra8_test_helper",
            f"function '{sym.name}' tagged RA8_TEST_HELPER called from non-test context",
        )
        for cs in ctx.callers_of(sym)
        if not is_test_path(cs.caller_file)
    ]


def _rule_internal(sym: AnnotatedSymbol, _arg: str, _ctx: RuleCtx) -> list[Violation]:
    """RA8_INTERNAL: the definition must actually be static."""
    if sym.is_static:
        return []
    return [
        _at(
            sym,
            "ra8_internal",
            f"function '{sym.name}' tagged RA8_INTERNAL is not declared static",
        )
    ]


def _rule_priv(sym: AnnotatedSymbol, _arg: str, ctx: RuleCtx) -> list[Violation]:
    """RA8_PRIV: callers must share the callee's libs/<module> or tools/<tool>."""
    callee_mod = module_of(sym.file)
    if callee_mod is None:
        return []
    out: list[Violation] = []
    for cs in ctx.callers_of(sym):
        # Host unit tests are a sanctioned consumer of a module's promoted
        # internals: CLAUDE.md ("Test access to internal symbols") allows a
        # helper to drop `static` and be declared in the module's _internal.h
        # precisely so tests can reach the validation paths for MC/DC.
        # Production callers in *other* modules remain violations.
        if is_test_path(cs.caller_file):
            continue
        caller_mod = module_of(cs.caller_file)
        if caller_mod != callee_mod:
            out.append(
                _at_call(
                    cs,
                    "ra8_priv",
                    f"function '{sym.name}' tagged RA8_PRIV ({callee_mod}) called "
                    f"from outside its module (caller={caller_mod or 'unknown'})",
                )
            )
    return out


def _rule_di_slot(sym: AnnotatedSymbol, _arg: str, ctx: RuleCtx) -> list[Violation]:
    """RA8_DI_SLOT: must be reached through a function pointer, not called."""
    direct = ctx.callers_of(sym)
    if not direct or sym.usr in ctx.address_taken:
        return []
    cs = direct[0]
    return [
        Violation(
            "ra8_di_slot",
            cs.caller_file,
            cs.caller_line,
            f"function '{sym.name}' tagged RA8_DI_SLOT called directly; "
            f"must be invoked via function pointer (DIP)",
            warn_only=True,  # heuristic; warn until pattern stable
        )
    ]


def _veneer_location(sym: AnnotatedSymbol) -> list[Violation]:
    """A veneer must be defined under libs/ra8_nsc/src/."""
    where = sym.file.replace("\\", "/")
    if _NSC_SRC_DIR in where:
        return []
    return [
        _at(
            sym,
            "ra8_nsc_veneer",
            f"NSC veneer '{sym.name}' must live under libs/ra8_nsc/src/ (found {where})",
        )
    ]


def _veneer_range_check(sym: AnnotatedSymbol) -> list[Violation]:
    """A veneer must range-check every address it accepts from Non-Secure.

    Veneers that take no pointer parameter cross no address and need no check.

    The check is a textual scan, not a call-graph lookup: the idiom is the
    RA8_NSC_CHECK_NS_RANGE_R/_RW macro, which expands to
    cmse_check_address_range() only under -mcmse. This script parses without
    -mcmse, so the macro expands to a ((void)(p),(void)(n)) no-op and leaves
    no CallExpr for libclang to find. The previous call-graph form looked for
    a callee named "ra8_nsc_check_*" -- a spelling no veneer has ever used --
    so it could only ever produce false positives.
    """
    if not sym.has_pointer_param or _NSC_RANGE_CHECK_RE.search(definition_text(sym)):
        return []
    return [
        _at(
            sym,
            "ra8_nsc_veneer",
            f"NSC veneer '{sym.name}' takes a pointer from NS but never "
            f"range-checks it (expected RA8_NSC_CHECK_NS_RANGE_R/_RW or "
            f"cmse_check_address_range)",
        )
    ]


def _veneer_section(sym: AnnotatedSymbol) -> list[Violation]:
    """A veneer must land in .gnu.sgstubs or be a cmse_nonsecure_entry."""
    if not sym.section or ".gnu.sgstubs" in sym.section:
        return []
    if "cmse_nonsecure_entry" in " ".join(sym.annotations):
        return []
    return [
        _at(
            sym,
            "ra8_nsc_veneer",
            f"NSC veneer '{sym.name}' must live in .gnu.sgstubs section or be cmse_nonsecure_entry",
        )
    ]


def _rule_nsc_veneer(sym: AnnotatedSymbol, _arg: str, _ctx: RuleCtx) -> list[Violation]:
    """RA8_NSC_VENEER: location, NS range-checking, and section placement."""
    return [*_veneer_location(sym), *_veneer_range_check(sym), *_veneer_section(sym)]


def _rule_hw_register_access(sym: AnnotatedSymbol, _arg: str, _ctx: RuleCtx) -> list[Violation]:
    """RA8_HW_REGISTER_ACCESS: an inline accessor returning a volatile pointer."""
    # Caller-side RA8_PROTECTED_WRITE / CITES-OK checking is left to a future
    # textual scan; libclang loses macro context that early.
    out: list[Violation] = []
    if not sym.has_inline:
        out.append(_at(sym, "ra8_hw_register_access", f"MMIO accessor '{sym.name}' must be inline"))
    if "volatile" not in sym.return_type:
        out.append(
            _at(
                sym,
                "ra8_hw_register_access",
                f"MMIO accessor '{sym.name}' must return volatile* (got '{sym.return_type}')",
            )
        )
    return out


def _rule_mcdc_deactivated(sym: AnnotatedSymbol, arg: str, _ctx: RuleCtx) -> list[Violation]:
    """RA8_MCDC_DEACTIVATED: the reason may not carry a file:line citation."""
    if not re.search(r"\.[ch]:\d+", arg):
        return []
    return [
        _at(
            sym,
            "ra8_mcdc_deactivated",
            f"ra8_mcdc_deactivated reason on '{sym.name}' contains file:line "
            f"citation -- use function name",
        )
    ]


def _rule_max_stack(sym: AnnotatedSymbol, arg: str, _ctx: RuleCtx) -> list[Violation]:
    """RA8_MAX_STACK: the measured .su frame must fit the declared budget."""
    try:
        annotated = int(arg)
    except ValueError:
        return [
            _at(
                sym,
                "ra8_max_stack",
                f"ra8_max_stack:'{arg}' on '{sym.name}' is not an integer byte count",
            )
        ]
    measured = find_su_file(sym)
    if measured is None or measured <= annotated:
        return []
    return [
        _at(
            sym,
            "ra8_max_stack",
            f"function '{sym.name}' frame {measured} B exceeds ra8_max_stack:{annotated}",
        )
    ]


def _rule_expects_lock(sym: AnnotatedSymbol, arg: str, ctx: RuleCtx) -> list[Violation]:
    """RA8_EXPECTS_LOCK: each caller must take the named lock first."""
    out: list[Violation] = []
    for cs in ctx.callers_of(sym):
        body = ctx.calls_by_caller.get(cs.caller_usr, [])
        has_take = any(
            c.callee_name == "RA8_TAKE_LOCK" and c.caller_line < cs.caller_line for c in body
        )
        if not has_take:
            out.append(
                _at_call(
                    cs,
                    "ra8_expects_lock",
                    f"call to '{sym.name}' (expects lock '{arg}') missing "
                    f'preceding RA8_TAKE_LOCK("{arg}")',
                )
            )
    return out


def _rule_host_friendly(sym: AnnotatedSymbol, _arg: str, ctx: RuleCtx) -> list[Violation]:
    """RA8_HOST_FRIENDLY: must not reach an MMIO accessor."""
    out: list[Violation] = []
    for cs in ctx.body_calls(sym):
        callee = ctx.symbols.get(cs.callee_usr)
        if callee and any(
            a.startswith(("ra8_hw_register_access", "RA8_HW_REGISTER_ACCESS"))
            for a in callee.annotations
        ):
            out.append(
                _at_call(
                    cs,
                    "ra8_host_friendly",
                    f"host-friendly '{sym.name}' calls MMIO accessor "
                    f"'{cs.callee_name}' (would break in sim)",
                )
            )
    return out


def _rule_no_recursion(sym: AnnotatedSymbol, _arg: str, ctx: RuleCtx) -> list[Violation]:
    """RA8_NO_RECURSION: the transitive call closure must not include self."""
    seen = {sym.usr}
    stack = [sym.usr]
    guard = 0
    while stack and guard < RECURSION_GUARD_LIMIT:
        guard += 1
        for cs in ctx.calls_by_caller.get(stack.pop(), []):
            if cs.in_address_of:
                continue
            if cs.callee_usr == sym.usr:
                return [
                    _at(
                        sym,
                        "ra8_no_recursion",
                        f"function '{sym.name}' tagged RA8_NO_RECURSION appears "
                        f"in its own transitive call closure",
                    )
                ]
            if cs.callee_usr not in seen:
                seen.add(cs.callee_usr)
                stack.append(cs.callee_usr)
    return []


def _function_body_text(sym: AnnotatedSymbol) -> str:
    """Return ``sym``'s brace-balanced body, or '' when it cannot be sliced.

    A crude textual slice on purpose: libclang loses for/while bounds through
    the macros this rule is about, so the check runs on source text.
    """
    try:
        src = pathlib.Path(sym.file).read_text(errors="ignore")
    except OSError:
        return ""
    m = re.search(re.escape(sym.name) + r"\s*\([^;]*\)\s*\{", src)
    if not m:
        return ""
    start = m.end() - 1
    depth = 0
    for i in range(start, len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[start:i]
    return ""


def _loop_headers(body: str) -> list[str]:
    """Return the full parenthesised header of every for/while in ``body``.

    The paren run is matched by counting depth rather than by a
    ``[^)]*`` regex. A condition that contains parentheses of its own --
    ``(i < n) && (i < k_max)``, or a cast such as ``(uint32_t)k_max`` --
    is extremely common, and a paren-blind slice truncates it at the first
    inner ``)``. That made the bound symbol invisible whenever it sat after
    one, so the rule reported a violation against a loop that was in fact
    bounded exactly as the annotation claimed.
    """
    out: list[str] = []
    for m in re.finditer(r"\b(?:for|while)\s*\(", body):
        depth = 0
        for i in range(m.end() - 1, len(body)):
            if body[i] == "(":
                depth += 1
            elif body[i] == ")":
                depth -= 1
                if depth == 0:
                    out.append(body[m.end() : i])
                    break
    return out


def _rule_bounded_loop(sym: AnnotatedSymbol, arg: str, _ctx: RuleCtx) -> list[Violation]:
    """RA8_BOUNDED_LOOP: every loop condition must name the bounding symbol."""
    body = _function_body_text(sym)
    if not body:
        return []
    return [
        _at(
            sym,
            "ra8_bounded_loop",
            f"loop in '{sym.name}' missing bound symbol '{arg}' in condition '{header.strip()}'",
        )
        for header in _loop_headers(body)
        if arg not in header
    ]


def _rule_validates(sym: AnnotatedSymbol, arg: str, ctx: RuleCtx) -> list[Violation]:
    """RA8_VALIDATES: at least N RA8_CHECK_* calls in the body."""
    try:
        need = int(arg)
    except ValueError:
        return []
    count = sum(1 for c in ctx.body_calls(sym) if c.callee_name.startswith("RA8_CHECK_"))
    if count >= need:
        return []
    return [
        _at(
            sym,
            "ra8_validates",
            f"function '{sym.name}' has {count} RA8_CHECK_* calls; "
            f"ra8_validates:{need} requires at least {need}",
        )
    ]


def _rule_owns_resource(sym: AnnotatedSymbol, arg: str, ctx: RuleCtx) -> list[Violation]:
    """RA8_OWNS_RESOURCE: the body must reach a matching release."""
    released = f"ra8_releases_resource:{arg}"
    for c in ctx.body_calls(sym):
        callee = ctx.symbols.get(c.callee_usr)
        if callee and released in callee.annotations:
            return []
    return [
        _at(
            sym,
            "ra8_owns_resource",
            f"function '{sym.name}' acquires '{arg}' but no matching {released} call found",
        )
    ]


def _rule_latency_budget(sym: AnnotatedSymbol, arg: str, _ctx: RuleCtx) -> list[Violation]:
    """RA8_LATENCY_BUDGET_NS: recorded until a WCET pass exists."""
    return [
        _at(
            sym,
            "ra8_latency_budget_ns",
            f"ra8_latency_budget_ns:{arg} on '{sym.name}' recorded; no WCET pass yet",
        )
    ]


def _rule_reviewed_by(sym: AnnotatedSymbol, arg: str, _ctx: RuleCtx) -> list[Violation]:
    """RA8_REVIEWED_BY: informational rollup into the safety report."""
    return [_at(sym, "ra8_reviewed_by", f"reviewed-by '{arg}' recorded for '{sym.name}'")]


def _rule_register_bank(sym: AnnotatedSymbol, arg: str, _ctx: RuleCtx) -> list[Violation]:
    """RA8_REGISTER_BANK: informational grouping of MMIO accessors."""
    return [_at(sym, "ra8_register_bank", f"register-bank '{arg}' recorded for '{sym.name}'")]


#: Rule key -> the function that judges one symbol carrying it.
#:
#: Three keys are deliberately absent, and their absence is the statement:
#: ``ra8_isr_safe`` and ``ra8_releases_resource`` are read by other rules
#: rather than checked on their own, and ``ra8_nasa_rule_3_ok`` is a waiver
#: consumed by the tree-wide allocation sweep below. Every key here and in
#: ANNOTATION_PREFIXES is cross-checked against ra8_attributes.h on every run.
RULE_CHECKS: dict[str, Callable[[AnnotatedSymbol, str, RuleCtx], list[Violation]]] = {
    "ra8_test_helper": _rule_test_helper,
    "ra8_internal": _rule_internal,
    "ra8_priv": _rule_priv,
    "ra8_di_slot": _rule_di_slot,
    "ra8_nsc_veneer": _rule_nsc_veneer,
    "ra8_hw_register_access": _rule_hw_register_access,
    "ra8_mcdc_deactivated": _rule_mcdc_deactivated,
    "ra8_max_stack": _rule_max_stack,
    "ra8_expects_lock": _rule_expects_lock,
    "ra8_host_friendly": _rule_host_friendly,
    "ra8_no_recursion": _rule_no_recursion,
    "ra8_bounded_loop": _rule_bounded_loop,
    "ra8_validates": _rule_validates,
    "ra8_owns_resource": _rule_owns_resource,
    "ra8_latency_budget_ns": _rule_latency_budget,
    "ra8_reviewed_by": _rule_reviewed_by,
    "ra8_register_bank": _rule_register_bank,
}


def sweep_dynamic_allocation(
    symbols: dict[str, AnnotatedSymbol], calls: list[CallSite]
) -> list[Violation]:
    """NASA P10 Rule 3: firmware allocates only inside a tagged function.

    A tree-wide sweep rather than a per-symbol rule, because the property is
    about call sites that carry no annotation at all -- the set a rule keyed
    on an annotation cannot see.
    """
    exempt = {
        sym.usr
        for sym in symbols.values()
        if any(a.startswith("ra8_nasa_rule_3_ok") for a in sym.annotations)
    }
    return [
        _at_call(
            cs,
            "ra8_nasa_rule_3_ok",
            f"call to '{cs.callee_name}' from '{cs.caller_name}' which is "
            f"not tagged RA8_NASA_RULE_3_OK",
        )
        for cs in calls
        # Host-only code is outside NASA P10 Rule 3 by construction: the
        # firmware image never contains these TUs. See is_host_only_path().
        if not cs.in_address_of
        and not is_host_only_path(cs.caller_file)
        and cs.callee_name in _ALLOCATORS
        and cs.caller_usr not in exempt
    ]


def enforce_rules(
    symbols: dict[str, AnnotatedSymbol],
    calls: list[CallSite],
    vector_entries: set[str],
    *,
    whole_tree: bool = True,
) -> list[Violation]:
    """Apply every annotation rule and return the findings."""
    ctx = RuleCtx.build(symbols, calls)
    out: list[Violation] = []

    # The linkage rule is defined over the whole tree: a handler tabled in
    # one TU, or an API declared in a header no parsed TU includes, cannot
    # be judged from a subset. Running it over an explicit file list would
    # invent violations rather than find them.
    if whole_tree:
        out.extend(enforce_linkage(symbols, vector_entries))

    for sym in symbols.values():
        for ann in sym.annotations:
            rule, arg = parse_annotation(ann)
            check = RULE_CHECKS.get(rule)
            if check is None:
                continue
            found = check(sym, arg, ctx)
            if rule in INFORMATIONAL_RULES:
                for v in found:
                    v.warn_only = True
            out.extend(found)

    out.extend(sweep_dynamic_allocation(symbols, calls))
    return out
