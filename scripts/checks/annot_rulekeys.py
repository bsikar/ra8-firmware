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
    ``RA8_INTERNAL_ANNOTATE("ra8_<rule>...")``; the rule key is the text
    up to the first colon.
    """
    try:
        text = ATTRIBUTES_HEADER.read_text(errors="ignore")
    except OSError:
        return set()
    return {m.group(1) for m in re.finditer(r'RA8_INTERNAL_ANNOTATE\(\s*"(ra8_[a-z0-9_]+)', text)}


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
