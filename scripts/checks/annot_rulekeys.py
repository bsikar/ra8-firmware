# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""The annotation vocabulary, and the proof that it matches what the macros emit.

This module is small on purpose.  It holds the set of rule keys the checker
dispatches on, and one cross-check -- :func:`check_rule_keys` -- that compares
that set against the strings ``ra8_attributes.h`` actually writes.

That cross-check is the reason the vocabulary is not just spelled inline where
it is used.  A rule keyed on a spelling no macro produces matches zero symbols
and reports zero violations forever, which is indistinguishable from a clean
tree.  Four rules in this checker were in exactly that state at once.  Keeping
the keys and their proof in one file makes the two impossible to edit apart.
"""

from __future__ import annotations

import ast
import pathlib
import re

from annot_model import Violation
from annot_scope import repo_root

#: Path to the single header that defines every annotation macro. The
#: rule-key self-check reads the strings straight out of it. Bound to the
#: real checkout: this file is a fixed part of the repository, and unlike
#: the scan roots it is never re-pointed at a synthetic tree.
ATTRIBUTES_HEADER = repo_root() / "libs" / "ra8_core" / "inc" / "ra8_attributes.h"

#: Rules that record information rather than assert a property. They are
#: reported but never fail the gate -- there is nothing for a developer to
#: fix, the entry exists so the value shows up in the build log.
INFORMATIONAL_RULES = {"ra8_latency_budget_ns", "ra8_reviewed_by", "ra8_register_bank"}

#: Annotation keys that are deliberately markers: the macro exists and is
#: applied, and NOTHING checks it. Declaring one here is how the gate states
#: that gap out loud instead of letting the key sit in ANNOTATION_PREFIXES
#: looking wired. Each entry cites the issue that will give it teeth, and
#: :func:`check_rule_coverage` fails once a rule implements or reads the key,
#: so a declaration cannot outlive the gap it describes.
MARKER_ONLY_RULES = {
    # ra8_isr_safe: applied to 70 sites, read by nothing. The closure it
    # implies needs the ISR-entry set derived from the vector tables and a
    # ruling on inline MMIO accessors first -- issue #1247.
    "ra8_isr_safe",
}

ANNOTATION_PREFIXES = (
    "ra8_test_helper",
    "ra8_internal",
    "ra8_priv",
    "ra8_di_slot",
    "ra8_nsc_veneer",
    # Every entry here must match exactly what the corresponding macro in
    # libs/ra8_core/inc/ra8_attributes.h emits; check_rule_keys() proves
    # that on every run. Four of these were once spelled as something no
    # macro produced ("ra8_hw_mmio", "ra8_p10_rule3_exception",
    # "ra8_stack_max", "ra8_latency_max_ns") and the rules keyed on them
    # matched nothing at all while reporting success.
    "ra8_hw_register_access",
    "ra8_nasa_rule_3_ok",
    "ra8_mcdc_deactivated",
    "ra8_max_stack",
    "ra8_isr_safe",
    "ra8_expects_lock",
    "ra8_host_friendly",
    "ra8_latency_budget_ns",
    "ra8_no_recursion",
    "ra8_bounded_loop",
    "ra8_validates",
    "ra8_owns_resource",
    "ra8_releases_resource",
    "ra8_reviewed_by",
    "ra8_register_bank",
)

#: The three linkage annotations a non-static function may carry.
LINKAGE_ANNOTATIONS = frozenset({"ra8_priv", "ra8_internal", "ra8_test_helper"})


def parse_annotation(ann: str) -> tuple[str, str]:
    """Split ``ra8_max_stack:512`` -> (``ra8_max_stack``, ``512``)."""
    if ":" in ann:
        rule, _, arg = ann.partition(":")
        return rule.strip(), arg.strip()
    return ann.strip(), ""


def emitted_annotation_keys() -> set[str]:
    """Return every annotation string ``ra8_attributes.h`` can emit.

    Read straight out of the header rather than restated here, because a
    restatement is what goes stale. Each macro expands through
    ``RA8_INTERNAL_ANNOTATE("ra8_<rule>...")``, or through the shared
    ``RA8_INTERNAL_ANNOTATE_ARG("ra8_<rule>:", arg)`` helper the macros
    that carry a value use; the rule key is the text up to the first colon.
    """
    try:
        text = ATTRIBUTES_HEADER.read_text(errors="ignore")
    except OSError:
        return set()
    pattern = r'RA8_INTERNAL_ANNOTATE(?:_ARG)?\(\s*"(ra8_[a-z0-9_]+)'
    return {m.group(1) for m in re.finditer(pattern, text)}


def check_rule_keys() -> list[Violation]:
    """Fail when a rule keys on a string no annotation macro emits.

    This is the failure mode that looks exactly like success. A rule
    keyed on a spelling nothing produces matches zero symbols and reports
    zero violations for as long as nobody checks, and four rules in this
    file were in that state at once: RA8_HW_REGISTER_ACCESS emits
    "ra8_hw_register_access" but rule 6 looked for "ra8_hw_mmio", and the
    NASA-rule-3, stack-budget and latency-budget rules each looked for a
    key their macro never wrote. Cross-checking both directions against
    the header makes the whole class impossible to reintroduce silently.
    """
    emitted = emitted_annotation_keys()
    if not emitted:
        return [
            Violation(
                "ra8_rule_keys",
                str(ATTRIBUTES_HEADER),
                0,
                "no RA8_INTERNAL_ANNOTATE() strings found -- the annotation "
                "header moved or changed shape, so every rule key is unverified",
            )
        ]
    known = set(ANNOTATION_PREFIXES)
    out: list[Violation] = []
    out.extend(
        Violation(
            "ra8_rule_keys",
            str(ATTRIBUTES_HEADER),
            0,
            f"rule key '{key}' is not emitted by any macro in ra8_attributes.h; "
            f"the rule keyed on it can never match",
        )
        for key in sorted(known - emitted)
    )
    out.extend(
        Violation(
            "ra8_rule_keys",
            str(ATTRIBUTES_HEADER),
            0,
            f"annotation '{key}' is emitted by a macro but no rule recognises it; "
            f"every use of that macro is silently ignored",
        )
        for key in sorted(emitted - known)
    )
    return out


def _rule_module_paths() -> list[pathlib.Path]:
    """Return the checker modules a rule can be implemented or read in.

    This module is excluded because it holds the vocabulary itself: every
    key appears here as a literal, so counting it would make every key look
    read. ``annot_selftest`` is excluded because its fixtures mention keys
    in synthetic source strings, and a fixture is not a rule.
    """
    here = pathlib.Path(__file__).resolve()
    return sorted(
        p
        for p in here.parent.glob("annot_*.py")
        if p.name not in {here.name, "annot_selftest.py"}
    )


def _string_literals(source: str) -> list[str]:
    """Return every string literal in ``source`` except docstrings.

    Comments and docstrings are excluded deliberately. A comment asserting
    that a key is read is exactly the failure this check exists to catch:
    ``annot_rules`` carried one for ``ra8_isr_safe`` while no rule read it.
    Only a literal the interpreter actually evaluates counts as a use, and
    f-string fragments count too -- ``_rule_owns_resource`` reads its
    companion key as ``f"ra8_releases_resource:{arg}"``.
    """
    try:
        tree = ast.parse(source)
    except SyntaxError:
        return []
    doc_nodes = set()
    for node in ast.walk(tree):
        if not isinstance(node, (ast.Module, ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)):
            continue
        body = getattr(node, "body", None)
        if not body:
            continue
        first = body[0]
        if isinstance(first, ast.Expr) and isinstance(first.value, ast.Constant):
            if isinstance(first.value.value, str):
                doc_nodes.add(id(first.value))
    return [
        node.value
        for node in ast.walk(tree)
        if isinstance(node, ast.Constant)
        and isinstance(node.value, str)
        and id(node) not in doc_nodes
    ]


def keys_read_by_rules(modules: list[pathlib.Path] | None = None) -> set[str]:
    """Return the annotation keys some rule module actually names in code."""
    paths = _rule_module_paths() if modules is None else modules
    seen: set[str] = set()
    for path in paths:
        try:
            source = path.read_text(errors="ignore")
        except OSError:
            continue
        for literal in _string_literals(source):
            seen.update(key for key in ANNOTATION_PREFIXES if literal.startswith(key))
    return seen


def check_rule_coverage(
    implemented: frozenset[str] | set[str],
    *,
    modules: list[pathlib.Path] | None = None,
) -> list[Violation]:
    """Fail when a recognised annotation key is wired to nothing.

    :func:`check_rule_keys` proves the vocabulary and the header agree on
    the SPELLINGS. It says nothing about whether a recognised key reaches
    any code, and that is a second way for an annotation to be silently
    decorative: the key is spelled correctly, sits in ANNOTATION_PREFIXES,
    the dispatch loop in ``annot_rules.enforce_rules`` looks it up, finds
    no entry and moves on. Every use of the macro is then ignored while the
    gate reports success, which is what ``ra8_isr_safe`` did while both
    ``ra8_attributes.h`` and the ``RULE_CHECKS`` comment claimed a
    call-graph walk enforced it (issue #1247).

    So each recognised key must be one of three things, and the third is a
    declaration rather than an implementation:

    * implemented in ``annot_rules.RULE_CHECKS`` (passed in as
      ``implemented``, to keep this module free of that import);
    * read by name in another rule module, derived from the sources rather
      than restated -- a restatement is what went stale here;
    * declared in :data:`MARKER_ONLY_RULES`, which says the gap exists.

    The reverse direction matters just as much: a marker declaration that
    outlives its gap makes the next reader believe the annotation is still
    unchecked, so a declared key that is now implemented or read fails too.
    """
    paths = _rule_module_paths() if modules is None else modules
    if not paths:
        return [
            Violation(
                "ra8_rule_coverage",
                str(pathlib.Path(__file__).resolve().parent),
                0,
                "no annot_*.py rule modules found -- the checker was reshaped or "
                "renamed, so no annotation key can be shown to be enforced",
            )
        ]
    read = keys_read_by_rules(paths)
    wired = set(implemented) | read
    out: list[Violation] = []
    out.extend(
        Violation(
            "ra8_rule_coverage",
            str(ATTRIBUTES_HEADER),
            0,
            f"annotation '{key}' has no entry in RULE_CHECKS, is read by no other "
            f"rule, and is not declared in MARKER_ONLY_RULES; every use of its "
            f"macro is ignored while the gate reports success",
        )
        for key in sorted(set(ANNOTATION_PREFIXES) - wired - MARKER_ONLY_RULES)
    )
    out.extend(
        Violation(
            "ra8_rule_coverage",
            str(pathlib.Path(__file__).resolve()),
            0,
            f"'{key}' is declared marker-only but is not in ANNOTATION_PREFIXES; "
            f"the declaration describes a key no macro emits",
        )
        for key in sorted(MARKER_ONLY_RULES - set(ANNOTATION_PREFIXES))
    )
    out.extend(
        Violation(
            "ra8_rule_coverage",
            str(pathlib.Path(__file__).resolve()),
            0,
            f"'{key}' is declared marker-only but a rule now implements or reads "
            f"it; drop the MARKER_ONLY_RULES entry so the gap is not advertised "
            f"after it was closed",
        )
        for key in sorted(MARKER_ONLY_RULES & wired)
    )
    return out
