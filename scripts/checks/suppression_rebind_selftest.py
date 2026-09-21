#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Both-direction selftests for sanctioned suppression ledger operations.

Imported by suppression_rebind.selftest() through a module binding so
neither direction executes partially initialized names at import time.
"""

from __future__ import annotations

import hashlib
from dataclasses import replace

import suppression_rebind as core
from suppression_identity import binding_payload
from suppression_ledger import LedgerRow
from suppression_model import Suppression


def _fixture_row(site: str, binding: str, ref: str, state: str = "retain") -> LedgerRow:
    """Build one synthetic ledger row."""
    return LedgerRow(2, site, binding, state, "r", "batch-1", ref)


def _fixture_evidence(raw: object) -> tuple[str, ...]:
    """Coerce fixture evidence to a string tuple."""
    if isinstance(raw, tuple) and all(isinstance(item, str) for item in raw):
        return raw
    return ("decision-line:11", "standard:x")


def _fixture_record(**fields: object) -> Suppression:
    """Build one synthetic inventory record with a consistent binding."""
    record = Suppression(
        path=str(fields.get("path", "src/a.c")),
        line=int(fields.get("line", 10)),
        column=int(fields.get("column", 1)),
        family=str(fields.get("family", "mcdc-deactivation")),
        tool=str(fields.get("tool", "llvm-cov")),
        rule=str(fields.get("rule", "deactivated-condition")),
        directive=str(fields.get("directive", "mcdc-deactivated")),
        scope=str(fields.get("scope", "decision-line:11")),
        provenance=str(fields.get("provenance", "inline-comment")),
        reason=str(fields.get("reason", "operands cannot vary")),
        owner=str(fields.get("owner", "first-party")),
        concerns=(),
        evidence=_fixture_evidence(fields.get("evidence")),
        match_count=int(fields.get("match_count", 1)),
        recommendation=str(fields.get("recommendation", "revalidate-invariant")),
        anchor=str(fields.get("anchor", "// mcdc-deactivated:")),
    )
    binding = hashlib.sha256(binding_payload(record, record.anchor)).hexdigest()
    return replace(record, site_id=str(fields.get("site_id", "a" * 64)), binding_sha256=binding)


def _selftest_move() -> list[str]:
    """Prove a harmless move rebinds and unrelated rows survive."""
    failures: list[str] = []
    record = _fixture_record()
    old_binding = hashlib.sha256(
        binding_payload(
            replace(
                record,
                scope="decision-line:19",
                evidence=("decision-line:19", "standard:x"),
            ),
            record.anchor,
        )
    ).hexdigest()
    ref = "review-decision src/a.c:18 mcdc-deactivated: operands cannot vary."
    row = _fixture_row(record.site_id, old_binding, ref)
    other = _fixture_row("b" * 64, "c" * 64, "review-decision other.")
    plan, reason = core.plan_rebind([record], [row, other], record.site_id)
    if plan is None:
        return [f"harmless move refused: {reason}"]
    if (plan.old_refs, plan.new_refs) != (("19",), ("11",)):
        failures.append("move plan reports wrong decision refs")
    if (plan.marker_old, plan.marker_new) != ("18", "10"):
        failures.append("move plan reports wrong marker refs")
    if plan.new_binding != record.binding_sha256 or plan.old_binding != old_binding:
        failures.append("move plan reports wrong bindings")
    header = "site_id\tbinding_sha256\tstate\trationale_id\tbatch_id\tevidence_ref\n"
    ledger = (
        f"{header}"
        f"{record.site_id}\t{old_binding}\tretain\tr\tbatch-1\t{ref}\n"
        f"{'b' * 64}\t{'c' * 64}\tretain\tr\tbatch-1\treview-decision other.\n"
    )
    batches = (
        "batches:\n"
        "  - id: batch-1\n"
        "    authority: repository-suppression-review\n"
        "    date: 2026-09-12\n"
        "    identity_schema: 2-durable-site-identity\n"
        "    assigned_rows: 2\n"
        f"    rows_sha256: {core.batch_digest([row, other])}\n"
    )
    result = core.apply_plan(ledger, batches, plan)
    if isinstance(result, str):
        return [f"harmless move apply refused: {result}"]
    new_ledger, new_batches = result
    if f"b{'b' * 63}" not in new_ledger or "review-decision other." not in new_ledger:
        failures.append("unrelated ledger row changed")
    old_lines = ledger.split("\n")
    new_lines = new_ledger.split("\n")
    if [line for line in new_lines if record.site_id not in line] != [
        line for line in old_lines if record.site_id not in line
    ]:
        failures.append("non-target ledger bytes changed")
    if record.binding_sha256 not in new_ledger or "src/a.c:10" not in new_ledger:
        failures.append("rebound row missing new binding or line ref")
    if "assigned_rows: 2" not in new_batches:
        failures.append("batch member count changed")
    return failures


def _selftest_retire() -> list[str]:
    """Prove stale successions retire and bad ones refuse."""
    failures: list[str] = []
    live = _fixture_record(
        path="scripts/checks/suppression_rebind_selftest.py", line=20, scope="decision-line:20"
    )
    stale_ref = (
        "review-decision scripts/checks/suppression_rebind_selftest.py:19 "
        "mcdc-deactivated: operands vary."
    )
    stale = LedgerRow(2, "d" * 64, live.binding_sha256, "retain", "r", "batch-1", stale_ref)
    req = core.RetireRequest(
        "d" * 64,
        live.site_id,
        "scripts/checks/suppression_rebind_selftest.py",
        "mcdc-deactivated",
        "doc move",
    )
    plan, reason = core.plan_retire([live], [stale], req)
    if plan is None:
        return [f"stale succession refused: {reason}"]
    if plan.successor != live.site_id or plan.batch_id != "batch-1":
        failures.append("retire plan misreports successor or batch")
    probe_req = core.RetireRequest(
        "d" * 64,
        live.site_id,
        "scripts/checks/suppression_ledger.py",
        "mcdc-deactivated",
        "doc move",
    )
    probe, _ = core.plan_retire([live], [stale], probe_req)
    if probe is not None:
        failures.append("coords mismatch retires")
    ledger = (
        "site_id\tbinding_sha256\tstate\trationale_id\tbatch_id\tevidence_ref\n"
        f"{'d' * 64}\t{live.binding_sha256}\tretain\tr\tbatch-1\t{stale_ref}\n"
    )
    batches = (
        "batches:\n"
        "  - id: batch-1\n"
        "    authority: repository-suppression-review\n"
        "    date: 2026-09-12\n"
        "    identity_schema: 2-durable-site-identity\n"
        "    assigned_rows: 1\n"
        f"    rows_sha256: {core.batch_digest([stale])}\n"
    )
    result = core.apply_retire(ledger, batches, plan)
    if isinstance(result, str):
        return [f"stale apply refused: {result}"]
    new_ledger, _ = result
    if "\tsuperseded\tsuperseded-identity-rebinding\t" not in new_ledger:
        failures.append("retire did not flip state and rationale")
    tokens = new_ledger.split()
    if f"replaced-by:{live.site_id}" not in tokens:
        failures.append("retire did not record a clean successor link")
    live_req = core.RetireRequest(live.site_id, live.site_id, "src/a.c", "m", "x")
    probe, _ = core.plan_retire([live], [stale], live_req)
    if probe is not None:
        failures.append("live site retires")
    return failures


def _selftest_relink_legacy(
    failures: list[str], case: tuple[Suppression, LedgerRow, LedgerRow, str, str]
) -> None:
    """Prove legacy separators normalize during relink."""
    live, row, dead, ref, batches = case
    legacy_ref = ref + ";;;"
    legacy_row = _fixture_row("e" * 64, live.binding_sha256, legacy_ref, state="superseded")
    legacy_req = core.RelinkRequest(
        "e" * 64, "d" * 64, live.site_id, "src/a.c", "mcdc-deactivated", "x"
    )
    legacy_plan, legacy_reason = core.plan_relink([live], [legacy_row, dead], legacy_req)
    if legacy_plan is None:
        failures.append(f"legacy separators refuse to plan: {legacy_reason}")
    else:
        legacy_ledger = (
            "site_id\tbinding_sha256\tstate\trationale_id\tbatch_id\tevidence_ref\n"
            f"{'e' * 64}\t{live.binding_sha256}\tsuperseded\tr\tbatch-1\t{legacy_ref}\n"
        )
        legacy_batches = batches.replace(core.batch_digest([row]), core.batch_digest([legacy_row]))
        legacy_result = core.apply_relink(legacy_ledger, legacy_batches, legacy_plan)
        if isinstance(legacy_result, str):
            failures.append(f"legacy separators refuse to apply: {legacy_result}")
        elif f"replaced-by:{live.site_id}" not in legacy_result[0].split():
            failures.append("legacy separators did not normalize to a clean link")


def _selftest_relink() -> list[str]:
    """Prove dangling links refresh and bad ones refuse."""
    failures: list[str] = []
    live = _fixture_record()
    ref = (
        f"review-decision src/a.c:19 mcdc-deactivated: operands cannot vary. replaced-by:{'d' * 64}"
    )
    row = _fixture_row("e" * 64, live.binding_sha256, ref, state="superseded")
    dead = _fixture_row("d" * 64, live.binding_sha256, ref)
    rows = [row, dead]
    req = core.RelinkRequest("e" * 64, "d" * 64, live.site_id, "src/a.c", "mcdc-deactivated", "x")
    plan, reason = core.plan_relink([live], rows, req)
    if plan is None:
        return [f"dangling link refused: {reason}"]
    if plan.new_target != live.site_id:
        failures.append("relink plan misreports the successor")
    ledger = (
        "site_id\tbinding_sha256\tstate\trationale_id\tbatch_id\tevidence_ref\n"
        f"{'e' * 64}\t{live.binding_sha256}\tsuperseded\tr\tbatch-1\t{ref}\n"
    )
    batches = (
        "batches:\n"
        "  - id: batch-1\n"
        "    authority: repository-suppression-review\n"
        "    date: 2026-09-12\n"
        "    identity_schema: 2-durable-site-identity\n"
        "    assigned_rows: 1\n"
        f"    rows_sha256: {core.batch_digest([row])}\n"
    )
    result = core.apply_relink(ledger, batches, plan)
    if isinstance(result, str):
        return [f"relink apply refused: {result}"]
    new_ledger, _ = result
    if f"replaced-by:{live.site_id}" not in new_ledger:
        failures.append("relink did not refresh the successor link")
    if f"replaced-by:{'d' * 64}" in new_ledger:
        failures.append("relink left the dead link behind")
    _selftest_relink_legacy(failures, (live, row, dead, ref, batches))
    stray = _fixture_row(
        "d" * 64,
        live.binding_sha256,
        "review scripts/checks/suppression_model.py:9 directive:other-kind rule:x.",
    )
    stray_req = core.RelinkRequest("e" * 64, "d" * 64, live.site_id, "src/a.c", "m", "x")
    plan, _ = core.plan_relink([live], [row, stray], stray_req)
    if plan is not None:
        failures.append("stale old-target coordinates relink")
    return failures


def _selftest_restore() -> list[str]:
    """Prove live resolved rows reinstate and drift refuses."""
    failures: list[str] = []
    record = _fixture_record()
    legacy = f"history replaced-by:{'e' * 64}; retirement note."
    row = LedgerRow(2, record.site_id, record.binding_sha256, "resolved", "r", "batch-1", legacy)
    rationales = {"r": {"state": "retain"}}
    plan, reason = core.plan_restore(
        [record], [row], rationales, core.RestoreRequest(record.site_id, "r", "back")
    )
    if plan is None:
        return [f"live resolved row refused: {reason}"]
    ledger = (
        "site_id\tbinding_sha256\tstate\trationale_id\tbatch_id\tevidence_ref\n"
        f"{record.site_id}\t{record.binding_sha256}\tresolved\tr\tbatch-1\t{legacy}\n"
    )
    batches = (
        "batches:\n"
        "  - id: batch-1\n"
        "    authority: repository-suppression-review\n"
        "    date: 2026-09-12\n"
        "    identity_schema: 2-durable-site-identity\n"
        "    assigned_rows: 1\n"
        f"    rows_sha256: {core.batch_digest([row])}\n"
    )
    result = core.apply_restore(ledger, batches, plan)
    if isinstance(result, str):
        return [f"restore apply refused: {result}"]
    new_ledger, _ = result
    if "\tretain\tr\t" not in new_ledger or "reinstated: back" not in new_ledger:
        failures.append("restore did not flip state and rationale")
    if "retirement note. reinstated: back" not in new_ledger:
        failures.append("restore note glued to trailing prose")
    drifted = replace(record, binding_sha256="0" * 64)
    drift_req = core.RestoreRequest(record.site_id, "r", "back")
    plan, _ = core.plan_restore([drifted], [row], rationales, drift_req)
    if plan is not None:
        failures.append("drifted binding restores")
    return failures


def _selftest_refusals() -> list[str]:
    """Prove semantic changes and scope violations fail closed."""
    failures: list[str] = []
    record = _fixture_record()
    ref = "review-decision src/a.c:19 mcdc-deactivated: operands cannot vary."
    old_binding = hashlib.sha256(
        binding_payload(
            replace(
                record,
                scope="decision-line:19",
                evidence=("decision-line:19", "standard:x"),
            ),
            record.anchor,
        )
    ).hexdigest()
    row = _fixture_row(record.site_id, old_binding, ref)
    changed = replace(record, reason="totally different justification")
    changed = replace(changed, site_id=record.site_id, binding_sha256="0" * 64)
    plan, _ = core.plan_rebind([changed], [row], record.site_id)
    if plan is not None:
        failures.append("semantic reason change rebinds")
    wrong_kind = replace(record, directive="other-kind", site_id=record.site_id)
    plan, _ = core.plan_rebind([wrong_kind], [row], record.site_id)
    if plan is not None:
        failures.append("different suppression kind rebinds")
    plan, _ = core.plan_rebind([record], [row], "d" * 64)
    if plan is not None:
        failures.append("unknown site_id rebinds")
    twin = replace(record, line=11)
    plan, _ = core.plan_rebind([record, twin], [row], record.site_id)
    if plan is not None:
        failures.append("ambiguous live site rebinds")
    draft = LedgerRow(2, record.site_id, old_binding, "unreviewed", "r", "batch-1", ref)
    plan, _ = core.plan_rebind([record], [draft], record.site_id)
    if plan is not None:
        failures.append("unreviewed site rebinds")
    return failures


def run_selftests() -> list[str]:
    """Run every rebind selftest; return failure strings."""
    failures: list[str] = []
    failures.extend(_selftest_move())
    failures.extend(_selftest_refusals())
    failures.extend(_selftest_retire())
    failures.extend(_selftest_relink())
    failures.extend(_selftest_restore())
    return failures
