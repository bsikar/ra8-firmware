#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Sanctioned rebind of a reviewed suppression binding after a harmless move.

A durable ``site_id`` survives line movement, but the reviewed
``binding_sha256`` commits to line-derived evidence such as
``decision-line:410``. Deleting an unrelated include above a reviewed marker
therefore invalidates its binding without changing the reviewed target.
Hand-editing the ledger to repair the hash is forbidden; this module is the
sanctioned mechanism instead.

A rebind succeeds only when every identity-critical fact still matches: the
site resolves exactly once in the live inventory and exactly once in the
ledger, the ledger row is ``retain``, the reviewed prose still names the
path, directive, and exact reason sentence, and the live binding recomputed
with the old line numbers equals the reviewed binding (proving the sole
difference is line movement). The tool then writes the new binding, refreshes
line references in the evidence prose, and recomputes the owning batch
digest. It never creates, approves, re-justifies, or re-dispositions a row,
never touches unrelated rows, and refuses when the owning batch does not
currently validate.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
from dataclasses import dataclass, replace
from pathlib import Path
from typing import cast

import suppression_rebind_selftest as rebind_selftest
from suppression_c_control_scan import PAIRING_WINDOW
from suppression_identity import binding_payload
from suppression_ledger import LEDGER_COLUMNS, LedgerRow, load_ledger, load_rationales
from suppression_model import Suppression
from suppression_scan import scan_repository


@dataclass(frozen=True)
class RebindPlan:
    """Reviewed move repair awaiting reviewer confirmation."""

    site_id: str
    path: str
    batch_id: str
    old_refs: tuple[str, ...]
    new_refs: tuple[str, ...]
    marker_old: str
    marker_new: str
    old_binding: str
    new_binding: str
    target: str
    ledger_line: int


def _decision_numbers(evidence: tuple[str, ...]) -> list[str]:
    """Return decision-line numbers in evidence order."""
    numbers: list[str] = []
    for item in evidence:
        match = re.fullmatch(r"decision-line:(\d+)", item)
        if match is not None:
            numbers.append(match.group(1))
    return numbers


def _prose_refs(evidence_ref: str, path: str) -> list[str]:
    """Return `<path>:<digits>` line references in prose order."""
    return re.findall(re.escape(path) + r":(\d+)", evidence_ref)


def _mapped_scope(scope: str, mapping: dict[str, str]) -> str:
    """Rewrite a decision-line scope through a new/old number mapping."""
    match = re.fullmatch(r"decision-line:(\d+)", scope)
    if match is not None and match.group(1) in mapping:
        return f"decision-line:{mapping[match.group(1)]}"
    return scope


def _mapped_evidence(evidence: tuple[str, ...], mapping: dict[str, str]) -> tuple[str, ...]:
    """Rewrite decision-line evidence through a new/old number mapping."""
    rebuilt: list[str] = []
    for item in evidence:
        match = re.fullmatch(r"decision-line:(\d+)", item)
        if match is not None and match.group(1) in mapping:
            rebuilt.append(f"decision-line:{mapping[match.group(1)]}")
        else:
            rebuilt.append(item)
    return tuple(rebuilt)


def _recomputed_binding(record: Suppression, mapping: dict[str, str]) -> str:
    """Recompute the binding with old line numbers restored."""
    trial = replace(
        record,
        scope=_mapped_scope(record.scope, mapping),
        evidence=_mapped_evidence(record.evidence, mapping),
    )
    return hashlib.sha256(binding_payload(trial, trial.anchor)).hexdigest()


def _single_live(records: list[Suppression], site_id: str) -> tuple[Suppression | None, str]:
    """Resolve exactly one live record or explain the refusal."""
    live = [record for record in records if record.site_id == site_id]
    if not live:
        return None, f"unknown site_id {site_id}"
    if len(live) > 1:
        return None, f"ambiguous site_id {site_id}: {len(live)} live rows"
    return live[0], ""


def _single_row(rows: list[LedgerRow], site_id: str) -> tuple[LedgerRow | None, str]:
    """Resolve exactly one ledger row or explain the refusal."""
    hit = [row for row in rows if row.site_id == site_id]
    if not hit:
        return None, f"site_id {site_id} has no ledger row"
    if len(hit) > 1:
        return None, f"ambiguous site_id {site_id}: {len(hit)} ledger rows"
    if hit[0].state != "retain":
        return None, f"ledger state is {hit[0].state}; only retain rows rebind"
    return hit[0], ""


def _check_prose(record: Suppression, row: LedgerRow) -> str:
    """Require reviewed prose to name the live semantic target."""
    if record.path not in row.evidence_ref:
        return "reviewed prose does not name the live path"
    if record.directive not in row.evidence_ref and not (
        record.family in row.evidence_ref and record.rule in row.evidence_ref
    ):
        return "reviewed prose does not name the live directive"
    if record.reason not in row.evidence_ref:
        return "reviewed prose does not contain the live reason"
    return ""


def _check_movement(
    record: Suppression, row: LedgerRow
) -> tuple[tuple[str, str, str, str] | None, str]:
    """Prove the binding differs only by rigid line movement."""
    new_decisions = _decision_numbers(record.evidence)
    markers = _prose_refs(row.evidence_ref, record.path)
    if len(new_decisions) != 1 or len(set(markers)) != 1:
        return None, "line-reference counts do not align"
    new_decision = new_decisions[0]
    marker_old = markers[0]
    floor = max(1, int(marker_old) - PAIRING_WINDOW)
    hits: list[str] = []
    for candidate in range(floor, int(marker_old) + PAIRING_WINDOW + 1):
        mapping = {new_decision: str(candidate)}
        if _recomputed_binding(record, mapping) == row.binding_sha256:
            hits.append(str(candidate))
    if not hits:
        return None, "binding differs beyond line movement"
    if len(hits) > 1:
        return None, "old decision line ambiguous"
    if record.line - int(marker_old) != int(new_decision) - int(hits[0]):
        return None, "marker and decision shifts disagree"
    return (marker_old, str(record.line), hits[0], new_decision), ""


def _refresh_prose(evidence_ref: str, path: str, plan: RebindPlan) -> str:
    """Rewrite the prose marker reference to the live marker line."""
    token = re.compile(re.escape(path) + r":(\d+)")
    old, new = plan.marker_old, plan.marker_new

    def swap(match: re.Match[str]) -> str:
        """Swap one prose reference for its planned successor."""
        return f"{path}:{new}" if match.group(1) == old else match.group(0)

    return token.sub(swap, evidence_ref)


def plan_rebind(
    records: list[Suppression], rows: list[LedgerRow], site_id: str
) -> tuple[RebindPlan | None, str]:
    """Plan a fail-closed rebind, or return the refusal reason."""
    record, problem = _single_live(records, site_id)
    if record is None:
        return None, problem
    row, problem = _single_row(rows, site_id)
    if row is None:
        return None, problem
    if record.binding_sha256 == row.binding_sha256:
        return None, "live binding already matches; nothing to rebind"
    problem = _check_prose(record, row)
    if problem:
        return None, problem
    found, problem = _check_movement(record, row)
    if found is None:
        return None, problem
    marker_old, marker_new, old_decision, new_decision = found
    return (
        RebindPlan(
            site_id=site_id,
            path=record.path,
            batch_id=row.batch_id,
            old_refs=(old_decision,),
            new_refs=(new_decision,),
            marker_old=marker_old,
            marker_new=marker_new,
            old_binding=row.binding_sha256,
            new_binding=record.binding_sha256,
            target=f"{record.directive}: {record.reason}",
            ledger_line=row.line,
        ),
        "",
    )


def _render_row(row: LedgerRow) -> str:
    """Serialize one ledger row in committed column order."""
    tab = "\t"
    return (
        f"{row.site_id}{tab}{row.binding_sha256}{tab}{row.state}"
        f"{tab}{row.rationale_id}{tab}{row.batch_id}{tab}{row.evidence_ref}"
    )


def batch_digest(members: list[LedgerRow]) -> str:
    """Recompute the ordered-row digest the gate validates."""
    payload = "\n".join(
        f"{item.site_id}\t{item.binding_sha256}\t{item.state}"
        f"\t{item.rationale_id}\t{item.evidence_ref}"
        for item in members
    )
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def _parse_ledger(
    ledger_text: str, site_id: str
) -> tuple[list[LedgerRow], list[str], LedgerRow | None, str]:
    """Parse ledger text and resolve one row, or explain the refusal."""
    lines = ledger_text.split("\n")
    header = "site_id\tbinding_sha256\tstate\trationale_id\tbatch_id\tevidence_ref"
    if not lines or lines[0] != header:
        return [], [], None, "ledger header mismatch"
    rows: list[LedgerRow] = []
    for number, raw in enumerate(lines[1:], start=2):
        if not raw.strip():
            continue
        parts = raw.split("\t")
        if len(parts) != LEDGER_COLUMNS:
            return [], [], None, f"ledger line {number} malformed"
        rows.append(LedgerRow(number, *parts))
    target = [row for row in rows if row.site_id == site_id]
    if len(target) != 1:
        return [], [], None, "ledger site ambiguous or missing"
    return rows, lines, target[0], ""


def _update_batch(
    batches_text: str, batch_id: str, rows: list[LedgerRow], members: list[LedgerRow]
) -> tuple[str, str]:
    """Rewrite one batch digest after validating it, or explain the refusal."""
    block = re.search(
        r"(?m)^  - id: " + re.escape(batch_id) + r"\n(?:^    \S.*\n|\n|(?:^      .*\n)+)+",
        batches_text,
    )
    if block is None:
        return "", f"batch {batch_id} not found"
    if len(re.findall(r"(?m)^  - id: " + re.escape(batch_id) + r"$", batches_text)) != 1:
        return "", f"batch {batch_id} ambiguous"
    current = batch_digest([item for item in rows if item.batch_id == batch_id])
    recorded = re.search(r"(?m)^    rows_sha256: ([0-9a-f]{64})$", block.group(0))
    if recorded is None or recorded.group(1) != current:
        return "", f"batch {batch_id} does not currently validate"
    fresh = batch_digest([item for item in members if item.batch_id == batch_id])
    return (
        batches_text[: block.start()]
        + block.group(0).replace(recorded.group(1), fresh, 1)
        + batches_text[block.end() :],
        "",
    )


def apply_plan(ledger_text: str, batches_text: str, plan: RebindPlan) -> tuple[str, str] | str:
    """Apply a plan to ledger/batches text, or return the refusal reason."""
    rows, lines, row, problem = _parse_ledger(ledger_text, plan.site_id)
    if problem:
        return problem
    if row is None:
        return "ledger site missing after parse"
    if row.binding_sha256 != plan.old_binding:
        return "ledger binding changed since planning"
    new_ref = _refresh_prose(row.evidence_ref, plan.path, plan)
    members = [
        LedgerRow(
            row.line,
            row.site_id,
            plan.new_binding,
            row.state,
            row.rationale_id,
            row.batch_id,
            new_ref,
        )
        if item.site_id == plan.site_id
        else item
        for item in rows
    ]
    new_batches, problem = _update_batch(batches_text, plan.batch_id, rows, members)
    if problem:
        return problem
    return _join_ledger(lines, members), new_batches


def apply_retire(ledger_text: str, batches_text: str, plan: RetirePlan) -> tuple[str, str] | str:
    """Apply a succession to ledger/batches text, or return the refusal reason."""
    rows, lines, row, problem = _parse_ledger(ledger_text, plan.site_id)
    if problem:
        return problem
    if row is None or row.state != "retain":
        return "ledger row changed since planning"
    ref = f"{row.evidence_ref} replaced-by:{plan.successor} succession: {plan.because}"
    if f"replaced-by:{plan.successor}" not in ref.split():
        return "retire wrote a malformed replaced-by link"
    members = [
        LedgerRow(
            row.line,
            row.site_id,
            row.binding_sha256,
            "superseded",
            "superseded-identity-rebinding",
            row.batch_id,
            ref,
        )
        if item.site_id == plan.site_id
        else item
        for item in rows
    ]
    new_batches, problem = _update_batch(batches_text, row.batch_id, rows, members)
    if problem:
        return problem
    return _join_ledger(lines, members), new_batches


def apply_relink(ledger_text: str, batches_text: str, plan: RelinkPlan) -> tuple[str, str] | str:
    """Apply a link refresh to ledger/batches text, or return the refusal reason."""
    rows, lines, row, problem = _parse_ledger(ledger_text, plan.site_id)
    if problem:
        return problem
    if row is None or row.state != "superseded":
        return "ledger row changed since planning"
    token = _recorded_token(row.evidence_ref, plan.old_target)
    if not token or row.evidence_ref.count(token) != 1:
        return "old target is not uniquely recorded in this row"
    ref = row.evidence_ref.replace(token, f"replaced-by:{plan.new_target}", 1)
    if f"replaced-by:{plan.new_target}" not in ref.split():
        return "relink wrote a malformed replaced-by link"
    members = [
        LedgerRow(
            row.line,
            row.site_id,
            row.binding_sha256,
            row.state,
            row.rationale_id,
            row.batch_id,
            ref,
        )
        if item.site_id == plan.site_id
        else item
        for item in rows
    ]
    new_batches, problem = _update_batch(batches_text, row.batch_id, rows, members)
    if problem:
        return problem
    return _join_ledger(lines, members), new_batches


def apply_restore(ledger_text: str, batches_text: str, plan: RestorePlan) -> tuple[str, str] | str:
    """Apply a reinstatement to ledger/batches text, or return the refusal reason."""
    rows, lines, row, problem = _parse_ledger(ledger_text, plan.site_id)
    if problem:
        return problem
    if row is None or row.state != "resolved":
        return "ledger row changed since planning"
    ref = f"{row.evidence_ref} reinstated: {plan.because}"
    members = [
        LedgerRow(
            row.line,
            row.site_id,
            row.binding_sha256,
            "retain",
            plan.rationale,
            row.batch_id,
            ref,
        )
        if item.site_id == plan.site_id
        else item
        for item in rows
    ]
    new_batches, problem = _update_batch(batches_text, row.batch_id, rows, members)
    if problem:
        return problem
    return _join_ledger(lines, members), new_batches


def _join_ledger(lines: list[str], members: list[LedgerRow]) -> str:
    """Re-emit ledger text with member rows substituted in place."""
    by_line = {item.line: _render_row(item) for item in members}
    out = [lines[0]]
    out.extend(by_line.get(number, lines[number - 1]) for number in range(2, len(lines) + 1))
    text = "\n".join(out)
    return text if text.endswith("\n") else text + "\n"


def _repo_root() -> Path:
    """Return the repository root housing this checker."""
    return Path(__file__).resolve().parents[2]


def _load_records(root: Path) -> tuple[list[Suppression], str]:
    """Load live inventory records through the repository scanner."""
    inventory, _ = scan_repository(root)
    return inventory.suppressions, ""


def _describe(plan: object) -> str:
    """Render a dry-run presentation of one plan."""
    if isinstance(plan, RebindPlan):
        return "\n".join(
            [
                f"site_id:     {plan.site_id}",
                f"path:        {plan.path}",
                f"batch:       {plan.batch_id}",
                f"old marker:  {plan.marker_old}",
                f"new marker:  {plan.marker_new}",
                f"old line:    {', '.join(plan.old_refs)}",
                f"new line:    {', '.join(plan.new_refs)}",
                f"old binding: {plan.old_binding}",
                f"new binding: {plan.new_binding}",
                f"target:      {plan.target}",
            ]
        )
    if isinstance(plan, RetirePlan):
        return "\n".join(
            [
                f"site_id:   {plan.site_id}",
                f"successor: {plan.successor}",
                f"target:    {plan.path} {plan.directive}",
                f"batch:     {plan.batch_id}",
                f"because:   {plan.because}",
            ]
        )
    if isinstance(plan, RelinkPlan):
        return "\n".join(
            [
                f"site_id:    {plan.site_id}",
                f"old target: {plan.old_target}",
                f"new target: {plan.new_target}",
                f"target:     {plan.path} {plan.directive}",
                f"batch:      {plan.batch_id}",
            ]
        )
    if isinstance(plan, RestorePlan):
        return "\n".join(
            [
                f"site_id:   {plan.site_id}",
                f"rationale: {plan.rationale}",
                f"batch:     {plan.batch_id}",
                f"because:   {plan.because}",
            ]
        )
    return f"unknown plan kind {type(plan)}"


def selftest() -> int:
    """Prove fail-closed ledger operations in both directions."""
    failures = rebind_selftest.run_selftests()
    if failures:
        for failure in failures:
            print(f"suppression_rebind selftest: FAILED -- {failure}", file=sys.stderr)
        return 1
    print("suppression_rebind selftest: all cases pass (both directions).")
    return 0


@dataclass(frozen=True)
class RetirePlan:
    """Reviewed succession for a stale retain row."""

    site_id: str
    successor: str
    path: str
    directive: str
    batch_id: str
    because: str
    ledger_line: int


@dataclass(frozen=True)
class RelinkPlan:
    """Reviewed successor refresh for a dangling replaced-by link."""

    site_id: str
    old_target: str
    new_target: str
    path: str
    directive: str
    batch_id: str
    because: str
    ledger_line: int


@dataclass(frozen=True)
class RestorePlan:
    """Reviewed reinstatement of a resolved row whose site is live."""

    site_id: str
    rationale: str
    batch_id: str
    because: str
    ledger_line: int


def _evidence_coords(evidence_ref: str) -> list[tuple[str, str]]:
    """Extract (path, directive) pairs named in reviewed prose."""
    return re.findall(r"(scripts/[A-Za-z_/]+\.py)(?::\d+)? directive:([A-Za-z_0-9]+)", evidence_ref)


def _replaced_by(evidence_ref: str) -> list[str]:
    """Extract replaced-by link targets in prose order."""
    return [
        part.removeprefix("replaced-by:")
        for part in evidence_ref.split()
        if part.startswith("replaced-by:")
    ]


def _clean_text(value: str) -> str:
    """Reject justification text that would break TSV structure."""
    if not value.strip() or "\t" in value or "\n" in value:
        return "justification must be non-blank single-line text"
    return ""


def _check_coords(coords: list[tuple[str, str]], path: str, directive: str, what: str) -> str:
    """Require an explicit target claim to match reviewed prose coordinates."""
    if coords and (path, directive) not in coords:
        return f"{what} does not match reviewed prose coordinates"
    return ""


@dataclass(frozen=True)
class RetireRequest:
    """Human-reviewed succession mapping for one stale retain row."""

    site_id: str
    successor: str
    path: str
    directive: str
    because: str


@dataclass(frozen=True)
class RelinkRequest:
    """Human-reviewed link refresh for one dangling replaced-by target."""

    site_id: str
    old_target: str
    new_target: str
    path: str
    directive: str
    because: str


@dataclass(frozen=True)
class RestoreRequest:
    """Human-reviewed reinstatement for one resolved row."""

    site_id: str
    rationale: str
    because: str


def _unique_row(rows: list[LedgerRow], site_id: str, want: str) -> tuple[LedgerRow | None, str]:
    """Resolve exactly one ledger row in the wanted state."""
    hit = [row for row in rows if row.site_id == site_id]
    if len(hit) != 1:
        return None, "ledger site ambiguous or missing"
    if hit[0].state != want:
        return None, f"ledger state is {hit[0].state}; want {want}"
    return hit[0], ""


def _unique_live(records: list[Suppression], site_id: str) -> tuple[Suppression | None, str]:
    """Resolve exactly one live record."""
    live = [record for record in records if record.site_id == site_id]
    if len(live) != 1:
        return None, "site unknown or ambiguous in live inventory"
    return live[0], ""


def _is_live(records: list[Suppression], site_id: str) -> bool:
    """Return whether one site resolves in the live inventory."""
    return any(record.site_id == site_id for record in records)


def _check_successor(records: list[Suppression], successor: str, path: str, directive: str) -> str:
    """Require a unique live successor matching the asserted target."""
    live, problem = _unique_live(records, successor)
    if live is None:
        return problem
    return _check_target(live, path, directive)


def _check_rationale(rationales: dict[str, dict[str, object]], name: str) -> str:
    """Require a known retain-graded rationale id."""
    spec = rationales.get(name)
    if not isinstance(spec, dict) or spec.get("state") != "retain":
        return f"rationale {name} unknown or not retain-graded"
    return ""


def _check_target(live: Suppression, path: str, directive: str) -> str:
    """Require a live successor to match the asserted target."""
    if (live.path, live.directive) != (path, directive):
        return "successor does not match the asserted target"
    return ""


def plan_retire(
    records: list[Suppression], rows: list[LedgerRow], req: RetireRequest
) -> tuple[RetirePlan | None, str]:
    """Plan a fail-closed succession for a stale retain row."""
    row, problem = _unique_row(rows, req.site_id, "retain")
    if row is None:
        return None, problem
    if _is_live(records, req.site_id):
        return None, "site is still live; use rebind or restore"
    problem = _check_successor(records, req.successor, req.path, req.directive)
    if problem:
        return None, problem
    problem = _check_coords(
        _evidence_coords(row.evidence_ref), req.path, req.directive, "asserted target"
    )
    if problem:
        return None, problem
    problem = _clean_text(req.because)
    if problem:
        return None, problem
    plan = RetirePlan(
        req.site_id, req.successor, req.path, req.directive, row.batch_id, req.because, row.line
    )
    return plan, ""


def _check_link_present(row: LedgerRow, old_target: str, new_target: str) -> str:
    """Require a recorded, non-trivial link refresh."""
    if old_target not in _replaced_by(row.evidence_ref):
        return "old target is not a recorded link of this row"
    if new_target == old_target:
        return "new target repeats the dead link"
    return ""


def _check_old_dead(
    records: list[Suppression], rows: list[LedgerRow], old_target: str, path: str, directive: str
) -> str:
    """Require a dead link target with a ledger row naming the same target."""
    if _is_live(records, old_target):
        return "old target is still live; link is not dangling"
    old_row = next((item for item in rows if item.site_id == old_target), None)
    if old_row is None:
        return "old target has no ledger row for continuity check"
    return _check_coords(_evidence_coords(old_row.evidence_ref), path, directive, "old target")


def _recorded_token(evidence_ref: str, old_target: str) -> str:
    """Find the prose token recording a link, tolerating legacy separators."""
    want = f"replaced-by:{old_target}"
    for token in evidence_ref.split():
        if token == want:
            return token
        if token.startswith(want) and set(token[len(want) :]) <= {";"}:
            return token
    return ""


def _check_link_prose(row: LedgerRow, req: RelinkRequest) -> str:
    """Require a recorded, non-trivial link refresh."""
    if not _recorded_token(row.evidence_ref, req.old_target):
        return "old target is not a recorded link of this row"
    if req.new_target == req.old_target:
        return "new target repeats the dead link"
    return ""


def _check_old_dead(records: list[Suppression], rows: list[LedgerRow], req: RelinkRequest) -> str:
    """Require a dead link target with a ledger row naming the same target."""
    if _is_live(records, req.old_target):
        return "old target is still live; link is not dangling"
    old_row = next((item for item in rows if item.site_id == req.old_target), None)
    if old_row is None:
        return "old target has no ledger row for continuity check"
    return _check_coords(_evidence_coords(old_row.evidence_ref), req.path, req.directive, "old")


def plan_relink(
    records: list[Suppression], rows: list[LedgerRow], req: RelinkRequest
) -> tuple[RelinkPlan | None, str]:
    """Plan a fail-closed successor refresh for a dangling link."""
    row, problem = _unique_row(rows, req.site_id, "superseded")
    if row is None:
        return None, problem
    problem = _check_link_prose(row, req)
    if problem:
        return None, problem
    problem = _check_successor(records, req.new_target, req.path, req.directive)
    if problem:
        return None, problem
    problem = _check_old_dead(records, rows, req)
    if problem:
        return None, problem
    plan = RelinkPlan(
        site_id=req.site_id,
        old_target=req.old_target,
        new_target=req.new_target,
        path=req.path,
        directive=req.directive,
        batch_id=row.batch_id,
        because=req.because,
        ledger_line=row.line,
    )
    return plan, ""


def _check_restore_live(
    records: list[Suppression], site_id: str, row: LedgerRow
) -> tuple[Suppression | None, str]:
    """Require a live record carrying the reviewed binding."""
    live, problem = _unique_live(records, site_id)
    if live is None:
        return None, problem
    if live.binding_sha256 != row.binding_sha256:
        return None, "live binding differs; needs new review, not restore"
    return live, ""


def plan_restore(
    records: list[Suppression],
    rows: list[LedgerRow],
    rationales: dict[str, dict[str, object]],
    req: RestoreRequest,
) -> tuple[RestorePlan | None, str]:
    """Plan a fail-closed reinstatement of a resolved row whose site is live."""
    row, problem = _unique_row(rows, req.site_id, "resolved")
    if row is None:
        return None, problem
    live, problem = _check_restore_live(records, req.site_id, row)
    if live is None:
        return None, problem
    problem = _check_rationale(rationales, req.rationale)
    if problem:
        return None, problem
    problem = _clean_text(req.because)
    if problem:
        return None, problem
    return RestorePlan(req.site_id, req.rationale, row.batch_id, req.because, row.line), ""


def _require_because(retires: list[str], restores: list[str], because: str | None) -> str:
    """Require recorded justification for state-changing operations."""
    if (retires or restores) and not because:
        return "--because is required"
    return ""


def _split_spec(value: str, parts: int, kind: str) -> tuple[list[str], str]:
    """Split one CLI op spec or report the shape violation."""
    fields = value.split(":")
    if len(fields) != parts or not all(fields):
        return [], f"malformed --{kind} spec (want {parts} colon fields)"
    return fields, ""


@dataclass(frozen=True)
class OpBatch:
    """One CLI invocation worth of ledger operations."""

    rebind: str | None
    retires: list[str]
    relinks: list[str]
    restores: list[str]
    rationale: str | None
    because: str | None
    write: bool


def _plan_jobs(
    records: list[Suppression],
    rows: list[LedgerRow],
    rationales: dict[str, dict[str, object]],
    batch: OpBatch,
) -> tuple[list[tuple[str, object]], str]:
    """Plan every requested operation against one inventory."""
    jobs: list[tuple[str, object]] = []
    if batch.rebind is not None:
        plan, problem = plan_rebind(records, rows, batch.rebind)
        if plan is None:
            return [], problem
        jobs.append(("rebind", plan))
    for adder in (
        _plan_retires(records, rows, batch),
        _plan_relinks(records, rows, batch),
        _plan_restores(records, rows, rationales, batch),
    ):
        plans, problem = adder
        if problem:
            return [], problem
        jobs.extend(plans)
    return jobs, ""


def _plan_retires(
    records: list[Suppression], rows: list[LedgerRow], batch: OpBatch
) -> tuple[list[tuple[str, object]], str]:
    """Plan every requested succession."""
    jobs: list[tuple[str, object]] = []
    for spec in batch.retires:
        fields, problem = _split_spec(spec, 4, "retire")
        if problem:
            return [], problem
        site, successor, path, directive = fields
        plan, problem = plan_retire(
            records, rows, RetireRequest(site, successor, path, directive, batch.because or "")
        )
        if plan is None:
            return [], problem
        jobs.append(("retire", plan))
    return jobs, ""


def _plan_relinks(
    records: list[Suppression], rows: list[LedgerRow], batch: OpBatch
) -> tuple[list[tuple[str, object]], str]:
    """Plan every requested link refresh."""
    jobs: list[tuple[str, object]] = []
    for spec in batch.relinks:
        fields, problem = _split_spec(spec, 5, "relink")
        if problem:
            return [], problem
        site, old, new, path, directive = fields
        plan, problem = plan_relink(
            records, rows, RelinkRequest(site, old, new, path, directive, batch.because or "")
        )
        if plan is None:
            return [], problem
        jobs.append(("relink", plan))
    return jobs, ""


def _plan_restores(
    records: list[Suppression],
    rows: list[LedgerRow],
    rationales: dict[str, dict[str, object]],
    batch: OpBatch,
) -> tuple[list[tuple[str, object]], str]:
    """Plan every requested reinstatement."""
    jobs: list[tuple[str, object]] = []
    for site in batch.restores:
        plan, problem = plan_restore(
            records,
            rows,
            rationales,
            RestoreRequest(site, batch.rationale or "", batch.because or ""),
        )
        if plan is None:
            return [], problem
        jobs.append(("restore", plan))
    return jobs, ""


def _apply_jobs(root: Path, jobs: list[tuple[str, object]]) -> tuple[str, str] | str:
    """Apply planned operations sequentially to ledger/batches text."""
    ledger_path = root / ".github" / "suppression-review-ledger.tsv"
    batches_path = root / ".github" / "suppression-review-batches.yml"
    ledger_text = ledger_path.read_text(encoding="utf-8")
    batches_text = batches_path.read_text(encoding="utf-8")
    for kind, plan in jobs:
        if kind == "rebind":
            result = apply_plan(ledger_text, batches_text, cast(RebindPlan, plan))
        elif kind == "retire":
            result = apply_retire(ledger_text, batches_text, cast(RetirePlan, plan))
        elif kind == "relink":
            result = apply_relink(ledger_text, batches_text, cast(RelinkPlan, plan))
        else:
            result = apply_restore(ledger_text, batches_text, cast(RestorePlan, plan))
        if isinstance(result, str):
            return result
        ledger_text, batches_text = result
    return ledger_text, batches_text


def _load_all(
    root: Path, batch: OpBatch
) -> tuple[tuple[list[Suppression], list[LedgerRow], dict[str, dict[str, object]]] | None, str]:
    """Load inventory, ledger, and rationales for one batch."""
    records, problem = _load_records(root)
    if problem:
        return None, problem
    rows, findings = load_ledger(root)
    if findings:
        return None, "ledger does not parse"
    rationales: dict[str, dict[str, object]] = {}
    if batch.restores:
        rationales, findings = load_rationales(root)
        if findings:
            return None, "rationales do not parse"
    return (records, rows, rationales), ""


def _run_ops(
    root: Path,
    batch: OpBatch,
) -> int:
    """Plan every requested operation against one inventory, then apply."""
    problem = _require_because(batch.retires, batch.restores, batch.because)
    if problem:
        return _refuse(problem)
    loaded, problem = _load_all(root, batch)
    if loaded is None:
        print(f"suppression_rebind: {problem}", file=sys.stderr)
        return 2
    records, rows, rationales = loaded
    jobs, problem = _plan_jobs(records, rows, rationales, batch)
    if problem:
        return _refuse(problem)
    for kind, plan in jobs:
        print(f"--- {kind} ---")
        print(_describe(plan))
    if not batch.write:
        return 0
    result = _apply_jobs(root, jobs)
    if isinstance(result, str):
        return _refuse(result)
    ledger_text, batches_text = result
    ledger_path = root / ".github" / "suppression-review-ledger.tsv"
    batches_path = root / ".github" / "suppression-review-batches.yml"
    ledger_path.write_text(ledger_text, encoding="utf-8")
    batches_path.write_text(batches_text, encoding="utf-8")
    print("suppression_rebind: applied.")
    return 0


def _refuse(reason: str) -> int:
    """Report one planning refusal."""
    print(f"suppression_rebind: refused: {reason}", file=sys.stderr)
    return 1


def main(argv: list[str] | None = None) -> int:
    """Offer dry-run review and fail-closed application of ledger operations."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--selftest", action="store_true", help="Run self-tests.")
    parser.add_argument("--site", help="Ledger site_id to rebind.")
    parser.add_argument("--retire", action="append", default=[], help="SITE:NEW:PATH:DIRECTIVE.")
    parser.add_argument(
        "--relink", action="append", default=[], help="SITE:OLD:NEW:PATH:DIRECTIVE."
    )
    parser.add_argument(
        "--restore", action="append", default=[], help="Ledger site_id to reinstate."
    )
    parser.add_argument("--rationale", help="Rationale id for --restore.")
    parser.add_argument("--because", help="Justification recorded with retire/restore.")
    parser.add_argument("--dry-run", action="store_true", help="Show the plan only.")
    parser.add_argument("--apply", action="store_true", help="Write the rebind.")
    args = parser.parse_args(argv)
    if args.selftest:
        return selftest()
    if args.restore and not args.rationale:
        parser.error("--restore requires --rationale")
    if not args.site and not args.retire and not args.relink and not args.restore:
        parser.error("--site, --retire, --relink, or --restore is required")
    if not args.dry_run and not args.apply:
        parser.error("--dry-run or --apply is required")
    return _run_ops(
        _repo_root(),
        OpBatch(
            args.site,
            args.retire,
            args.relink,
            args.restore,
            args.rationale,
            args.because,
            args.apply,
        ),
    )


if __name__ == "__main__":
    sys.exit(main())
