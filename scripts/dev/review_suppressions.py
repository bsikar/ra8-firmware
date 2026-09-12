#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Review Suppressions Utility.

Finds all 'unreviewed' ledger rows, assigns them a rationale, creates a batch,
and sets them to 'retain' so they pass CI.
"""

import argparse
import ast
import datetime
import hashlib
import json
import re
import subprocess
import sys
from collections.abc import Callable
from contextlib import suppress
from dataclasses import dataclass
from pathlib import Path
from typing import NoReturn

import yaml


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _read_ledger() -> list[str]:
    return (
        (_repo_root() / ".github" / "suppression-review-ledger.tsv")
        .read_text(encoding="utf-8")
        .splitlines()
    )


def _write_ledger(lines: list[str]) -> None:
    (_repo_root() / ".github" / "suppression-review-ledger.tsv").write_text(
        "\n".join(lines) + "\n", encoding="utf-8"
    )


def _get_rationale_evidence(args: argparse.Namespace) -> tuple[str, str]:
    rationale = args.rationale
    if not rationale:
        print("\nAvailable Rationales:")
        print("  1. tool-false-positive")
        print("  2. language-or-compiler-contract")
        print("  3. negative-test-contract")
        print("  4. generated-reproducible-output")
        print("  5. vendor-upstream-preserved")
        print("  6. reviewed-scope-authority")
        choice = input("Select a rationale (1-6) or type the ID directly: ").strip()
        mapping = {
            "1": "tool-false-positive",
            "2": "language-or-compiler-contract",
            "3": "negative-test-contract",
            "4": "generated-reproducible-output",
            "5": "vendor-upstream-preserved",
            "6": "reviewed-scope-authority",
        }
        rationale = mapping.get(choice, choice)

    evidence = args.evidence
    if not evidence:
        evidence = input(f"Enter evidence reference for why '{rationale}' applies here: ").strip()

    return rationale, evidence


def _write_batch(batch_id: str, count: int, batch_sha256: str, evidence: str) -> None:
    batches_path = _repo_root() / ".github" / "suppression-review-batches.yml"
    batches_yaml = batches_path.read_text(encoding="utf-8").rstrip()

    batch_entry = f"""
  - id: {batch_id}
    authority: repository-suppression-review
    date: {datetime.datetime.now(tz=datetime.UTC).strftime("%Y-%m-%d")}
    identity_schema: 2-durable-site-identity
    assigned_rows: {count}
    rows_sha256: {batch_sha256}
    partition: "Automated developer review batch"
    evidence:
      - "{evidence}"
"""
    batches_path.write_text(batches_yaml + batch_entry, encoding="utf-8")


def _build_parser() -> argparse.ArgumentParser:
    """Build the review CLI: legacy blanket flow plus selective subcommands."""
    parser = argparse.ArgumentParser(description="Approve draft unreviewed suppressions.")
    parser.add_argument("--rationale", type=str, help="Rationale ID")
    parser.add_argument("--evidence", type=str, help="Evidence reference string")
    parser.add_argument(
        "--selftest",
        action="store_true",
        help="run the selective review selftest (no ledger changes)",
    )
    subparsers = parser.add_subparsers(dest="action")
    retain_parser = subparsers.add_parser(
        "retain", help="retain explicitly selected live unreviewed rows"
    )
    retain_parser.add_argument("--site", action="append", default=[], help="site_id to retain")
    retain_parser.add_argument("--rationale", required=True, help="Rationale ID")
    retain_parser.add_argument("--evidence", required=True, help="Evidence reference string")
    retain_parser.add_argument(
        "--dry-run", action="store_true", help="print transitions without writing"
    )
    retire_parser = subparsers.add_parser(
        "retire", help="retire explicitly selected stale unreviewed rows"
    )
    retire_parser.add_argument("--site", action="append", default=[], help="site_id to retire")
    retire_parser.add_argument("--rationale", required=True, help="Rationale ID")
    retire_parser.add_argument("--evidence", required=True, help="Evidence reference string")
    retire_parser.add_argument(
        "--replaced-by",
        action="append",
        default=[],
        help="live successor site_id (supersede instead of resolve)",
    )
    retire_parser.add_argument(
        "--dry-run", action="store_true", help="print transitions without writing"
    )
    return parser


def main() -> int:
    """Execute the review approval flow."""
    args = _build_parser().parse_args()

    if args.selftest:
        return run_selftest()

    if args.action in ("retain", "retire"):
        return run_selective(args)

    ledger = _read_ledger()
    header = ledger[0]
    rows = ledger[1:]

    unreviewed = []
    for r in rows:
        if not r.strip():
            continue
        parts = r.split("\t")
        if parts[2] == "unreviewed":
            unreviewed.append(parts)

    if not unreviewed:
        print("No 'unreviewed' rows found in ledger. Nothing to do.")
        return 0

    print(f"Found {len(unreviewed)} unreviewed suppression(s).")
    rationale, evidence = _get_rationale_evidence(args)

    date_str = datetime.datetime.now(tz=datetime.UTC).strftime("%Y%m%d")
    batch_id = f"auto-review-batch-{date_str}"

    payload = [f"{parts[0]}\t{parts[1]}\tretain\t{rationale}\t{evidence}" for parts in unreviewed]

    batch_sha256 = hashlib.sha256("\n".join(payload).encode("utf-8")).hexdigest()

    updated_ledger = [header]
    for r in rows:
        if not r.strip():
            continue
        parts = r.split("\t")
        if parts[2] == "unreviewed":
            parts[2] = "retain"
            parts[3] = rationale
            parts[4] = batch_id
            parts[5] = evidence
            updated_ledger.append("\t".join(parts))
        else:
            updated_ledger.append(r)

    _write_ledger(updated_ledger)
    _write_batch(batch_id, len(unreviewed), batch_sha256, evidence)

    print(f"\nSuccessfully approved {len(unreviewed)} row(s) and generated batch {batch_id}.")
    print("Run `just quality::local::gate suppressions` to verify.")
    return 0


_LEDGER_HEADER = "site_id\tbinding_sha256\tstate\trationale_id\tbatch_id\tevidence_ref"
_LEDGER_COLUMNS = 6
_LEDGER_STATES = frozenset({"unreviewed", "retain", "fix-required", "resolved", "superseded"})
_HEX64_RE = re.compile(r"^[0-9a-f]{64}$")
_BATCHES_PATH = ".github/suppression-review-batches.yml"
_RATIONALES_PATH = ".github/suppression-review-rationales.yml"
_IDENTITY_MODULE = "scripts/checks/suppression_identity.py"
_IDENTITY_ATTR = "IDENTITY_SCHEMA_VERSION"


class ReviewError(ValueError):
    """A selective review action was refused fail-closed."""


def _refuse(message: str) -> NoReturn:
    """Reject one review action with a fail-closed diagnostic."""
    raise ReviewError(message)


def _parse_ledger(text: str) -> tuple[str, list[list[str]]]:
    """Split ledger text into header plus validated rows, failing closed."""
    lines = text.splitlines()
    if not lines or lines[0] != _LEDGER_HEADER:
        _refuse("malformed ledger: missing or wrong header")
    rows: list[list[str]] = []
    for lineno, line in enumerate(lines[1:], start=2):
        if not line.strip():
            continue
        parts = line.split("\t")
        if len(parts) != _LEDGER_COLUMNS:
            _refuse(f"malformed ledger: line {lineno} has {len(parts)} columns")
        site, binding, state = parts[0], parts[1], parts[2]
        if not _HEX64_RE.match(site):
            _refuse(f"malformed ledger: line {lineno} has a bad site_id")
        if not _HEX64_RE.match(binding):
            _refuse(f"malformed ledger: line {lineno} has a bad binding_sha256")
        if state not in _LEDGER_STATES:
            _refuse(f"malformed ledger: line {lineno} has unknown state {state!r}")
        if state == "unreviewed" and (parts[3] or parts[4]):
            _refuse(f"malformed ledger: line {lineno} unreviewed row carries rationale/batch")
        rows.append(parts)
    return lines[0], rows


def _rationale_states(root: Path) -> dict[str, str]:
    """Map each rationale category to its allowed ledger state."""
    try:
        data = yaml.safe_load((root / _RATIONALES_PATH).read_text(encoding="utf-8"))
    except OSError as exc:
        _refuse(f"cannot read rationale vocabulary: {exc}")
    except yaml.YAMLError as exc:
        _refuse(f"malformed rationale vocabulary: {exc}")
    try:
        categories = data["categories"]
        return {name: spec["state"] for name, spec in categories.items()}
    except (TypeError, KeyError, AttributeError) as exc:
        _refuse(f"malformed rationale vocabulary: {exc}")


def _identity_schema(root: Path) -> str:
    """Read the batch identity-schema constant without importing check code."""
    try:
        tree = ast.parse((root / _IDENTITY_MODULE).read_text(encoding="utf-8"))
    except OSError as exc:
        _refuse(f"cannot read identity module: {exc}")
    except SyntaxError as exc:
        _refuse(f"malformed identity module: {exc}")
    for node in ast.walk(tree):
        if (
            isinstance(node, ast.Assign)
            and len(node.targets) == 1
            and isinstance(node.targets[0], ast.Name)
            and node.targets[0].id == _IDENTITY_ATTR
            and isinstance(node.value, ast.Constant)
            and isinstance(node.value.value, str)
        ):
            return node.value.value
    _refuse("identity schema constant not found")


def _existing_batch_ids(root: Path) -> set[str]:
    """Return every committed batch id, failing closed on a broken file."""
    try:
        data = yaml.safe_load((root / _BATCHES_PATH).read_text(encoding="utf-8"))
    except OSError as exc:
        _refuse(f"cannot read batches file: {exc}")
    except yaml.YAMLError as exc:
        _refuse(f"malformed batches file: {exc}")
    try:
        return {str(record["id"]) for record in data["batches"]}
    except (TypeError, KeyError, AttributeError) as exc:
        _refuse(f"malformed batches file: {exc}")


def _live_bindings(root: Path) -> dict[str, str]:
    """Map every live site_id to its current binding via the canonical scan."""
    proc = subprocess.run(
        [
            sys.executable,
            "scripts/checks/check_suppressions.py",
            "--inventory",
            "--format",
            "json",
        ],
        cwd=root,
        capture_output=True,
        text=True,
        check=False,
    )
    try:
        inventory = json.loads(proc.stdout)
        return {item["site_id"]: item["binding_sha256"] for item in inventory["suppressions"]}
    except (json.JSONDecodeError, KeyError, TypeError) as exc:
        _refuse(f"live inventory scan failed: {exc}")


def _check_selection(site_ids: list[str], rows: list[list[str]]) -> list[list[str]]:
    """Resolve each requested id to exactly one ledger row, failing closed."""
    if not site_ids:
        _refuse("no --site given: selection is required")
    seen: set[str] = set()
    selected: list[list[str]] = []
    for site in site_ids:
        if not _HEX64_RE.match(site):
            _refuse(f"invalid site_id {site!r}: not a 64-hex identity")
        if site in seen:
            _refuse(f"ambiguous selection: {site[:12]}... requested twice")
        seen.add(site)
        matches = [row for row in rows if row[0] == site]
        if not matches:
            _refuse(f"unknown site_id {site[:12]}...: no ledger row")
        if len(matches) > 1:
            _refuse(f"ambiguous site_id {site[:12]}...: {len(matches)} ledger rows")
        selected.append(matches[0])
    return selected


@dataclass(frozen=True)
class ReviewContext:
    """Shared review inputs: rationale, evidence, live bindings, vocabulary."""

    rationale: str
    evidence: str
    live: dict[str, str]
    states: dict[str, str]


def _check_tsv_cell(field: str, value: str) -> None:
    """Reject TSV-breaking characters in a ledger-serialized review field."""
    for bad, name in (("\t", "tab"), ("\n", "newline"), ('"', "double-quote")):
        if bad in value:
            _refuse(f"{field} must be a single line without {name}: TSV cell integrity")


def _check_reviewable(selected: list[list[str]], ctx: ReviewContext) -> None:
    """Validate shared review preconditions for every selected row."""
    if not ctx.rationale.strip():
        _refuse("missing rationale: --rationale is required")
    if not ctx.evidence.strip():
        _refuse("missing evidence: --evidence is required")
    _check_tsv_cell("rationale", ctx.rationale)
    _check_tsv_cell("evidence", ctx.evidence)
    if ctx.rationale not in ctx.states:
        _refuse(f"unknown rationale {ctx.rationale!r}")
    for row in selected:
        if row[2] != "unreviewed":
            _refuse(f"already reviewed: {row[0][:12]}... is {row[2]!r}, not unreviewed")


def _require_state(ctx: ReviewContext, want: str) -> None:
    """Require the rationale to allow the target state, failing closed."""
    if ctx.states[ctx.rationale] != want:
        _refuse(f"rationale {ctx.rationale!r} does not allow state {want!r}")


@dataclass(frozen=True)
class BatchSpec:
    """One batch record's header plus its covered member rows."""

    batch_id: str
    date: str
    schema: str
    action: str
    members: list[list[str]]
    digest: str
    evidence: str


@dataclass(frozen=True)
class PlannedTransition:
    """One validated review transition with its pre-mutation state preserved."""

    row: list[str]
    old_state: str
    new_state: str
    rationale: str
    evidence: str
    note: str


def _plan_retain(selected: list[list[str]], ctx: ReviewContext) -> list[PlannedTransition]:
    """Validate a retain; preserve each row's pre-mutation state."""
    _check_reviewable(selected, ctx)
    _require_state(ctx, "retain")
    planned = []
    for row in selected:
        site, binding = row[0], row[1]
        if site not in ctx.live:
            _refuse(f"stale site {site[:12]}...: no live suppression; cannot retain")
        if ctx.live[site] != binding:
            _refuse(f"stale binding {site[:12]}...: ledger no longer matches live; cannot retain")
        planned.append(
            PlannedTransition(
                row, row[2], "retain", ctx.rationale, ctx.evidence, "retain live site"
            )
        )
    return planned


def _plan_retire(
    selected: list[list[str]], ctx: ReviewContext, replaced_by: list[str]
) -> list[PlannedTransition]:
    """Validate a retire; resolved needs a gone site, superseded live successors."""
    for successor in replaced_by:
        if not _HEX64_RE.match(successor):
            _refuse(f"invalid successor {successor!r}: not a 64-hex identity")
    _check_reviewable(selected, ctx)
    target = "superseded" if replaced_by else "resolved"
    _require_state(ctx, target)
    planned = []
    for row in selected:
        site = row[0]
        if site in ctx.live:
            _refuse(f"live site {site[:12]}...: retire refuses a standing suppression")
        if target == "resolved":
            planned.append(
                PlannedTransition(
                    row, row[2], target, ctx.rationale, ctx.evidence, "resolve vanished site"
                )
            )
            continue
        if site in replaced_by:
            _refuse(f"successor loop {site[:12]}...: cannot replace itself")
        for successor in replaced_by:
            if successor not in ctx.live:
                _refuse(f"dead successor {successor[:12]}...: replacement is not live")
        tokens = " ".join(f"replaced-by:{entry}" for entry in replaced_by)
        planned.append(
            PlannedTransition(
                row, row[2], target, ctx.rationale, f"{ctx.evidence} {tokens}", "supersede"
            )
        )
    return planned


def _new_batch_id(existing: set[str], now: datetime.datetime) -> str:
    """Mint a unique selective-review batch id, failing closed on collision."""
    candidate = f"selective-review-{now.strftime('%Y%m%d-%H%M%S')}"
    if candidate in existing:
        _refuse(f"batch id collision {candidate!r}: retry the review action")
    return candidate


def _batch_digest(members: list[list[str]]) -> str:
    """Digest ordered member rows exactly like the gate's batch check."""
    payload = "\n".join(f"{row[0]}\t{row[1]}\t{row[2]}\t{row[3]}\t{row[5]}" for row in members)
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def _batch_record(spec: BatchSpec) -> str:
    """Render one batch record in the committed batches-file shape."""
    sites = ", ".join(row[0][:12] for row in spec.members)
    return f"""
  - id: {spec.batch_id}
    authority: repository-suppression-review
    date: {spec.date}
    identity_schema: {spec.schema}
    assigned_rows: {len(spec.members)}
    rows_sha256: {spec.digest}
    partition: "selective {spec.action} of {len(spec.members)} ledger row(s): {sites}"
    evidence:
      - "{spec.evidence}"
"""


def _describe(planned: list[PlannedTransition], batch_id: str) -> list[str]:
    """Render one human-readable line per intended transition."""
    return [
        f"{item.note}: {item.row[0]} {item.old_state} -> {item.new_state} "
        f"rationale={item.rationale} batch={batch_id}"
        for item in planned
    ]


def run_selective(args: argparse.Namespace) -> int:
    """Execute one selective retain or retire review action."""
    root = _repo_root()
    try:
        header, rows = _parse_ledger("\n".join(_read_ledger()) + "\n")
        ctx = ReviewContext(
            rationale=args.rationale,
            evidence=args.evidence,
            live=_live_bindings(root),
            states=_rationale_states(root),
        )
        schema = _identity_schema(root)
        existing_batches = _existing_batch_ids(root)
        if args.action == "retain":
            planned = _plan_retain(_check_selection(args.site, rows), ctx)
        else:
            planned = _plan_retire(_check_selection(args.site, rows), ctx, args.replaced_by)
        now = datetime.datetime.now(tz=datetime.UTC)
        batch_id = _new_batch_id(existing_batches, now)
        for item in planned:
            item.row[2], item.row[3], item.row[4], item.row[5] = (
                item.new_state,
                item.rationale,
                batch_id,
                item.evidence,
            )
        wanted = {item.row[0] for item in planned}
        members = [row for row in rows if row[0] in wanted]
        digest = _batch_digest(members)
        if args.dry_run:
            print(f"dry-run {args.action}: no files written")
            for line in _describe(planned, batch_id):
                print(f"  {line}")
            print(f"  batch {batch_id} would cover {len(members)} row(s)")
            return 0
        _write_ledger([header] + ["\t".join(row) for row in rows])
        batches_path = root / _BATCHES_PATH
        batches_yaml = batches_path.read_text(encoding="utf-8").rstrip()
        spec = BatchSpec(
            batch_id=batch_id,
            date=now.strftime("%Y-%m-%d"),
            schema=schema,
            action=args.action,
            members=members,
            digest=digest,
            evidence=args.evidence,
        )
        batches_path.write_text(batches_yaml + _batch_record(spec), encoding="utf-8")
    except ReviewError as exc:
        print(f"review_suppressions.py: refused: {exc}", file=sys.stderr)
        return 1
    for line in _describe(planned, batch_id):
        print(line)
    print(f"wrote batch {batch_id} covering {len(members)} row(s)")
    print("Run `just quality::local::gate suppressions` to verify.")
    return 0


def _fixture_ledger() -> str:
    """Build a synthetic ledger exercising every transition direction."""
    rows = [
        f"{'a' * 64}\t{'1' * 64}\tunreviewed\t\t\t",
        f"{'b' * 64}\t{'2' * 64}\tunreviewed\t\t\t",
        f"{'c' * 64}\t{'3' * 64}\tunreviewed\t\t\t",
        f"{'d' * 64}\t{'4' * 64}\tretain\treviewed-scope-authority\tbatch-old\told evidence",
        f"{'e' * 64}\t{'5' * 64}\tresolved\tresolved-construct-retired\tbatch-old\told evidence",
        f"{'f' * 64}\t{'6' * 64}\tunreviewed\t\t\t",
    ]
    return _LEDGER_HEADER + "\n" + "\n".join(rows) + "\n"


def _fixture_live() -> dict[str, str]:
    """Live map: A/B/D live with matching bindings, F live with drifted binding."""
    return {
        "a" * 64: "1" * 64,
        "b" * 64: "2" * 64,
        "d" * 64: "4" * 64,
        "f" * 64: "f" * 64,
    }


def _fixture_states() -> dict[str, str]:
    """Rationale vocabulary matching the committed categories under review."""
    return {
        "reviewed-scope-authority": "retain",
        "tool-false-positive": "retain",
        "resolved-construct-retired": "resolved",
        "superseded-identity-rebinding": "superseded",
    }


def _ctx(
    rationale: str, evidence: str, live: dict[str, str], states: dict[str, str]
) -> ReviewContext:
    """Build one review context for selftest fixtures."""
    return ReviewContext(rationale=rationale, evidence=evidence, live=live, states=states)


def _refused(failures: list[str], label: str, func: Callable[..., object], *args: object) -> None:
    """Assert one guard refuses; record a failure when it accepts."""
    try:
        func(*args)
    except ReviewError:
        return
    failures.append(f"{label}: expected refusal, action was accepted")


def _test_retain(failures: list[str], a: str, b: str) -> None:
    """Prove selective retain of one and many rows with byte-exact isolation."""
    live, states = _fixture_live(), _fixture_states()
    _header, rows = _parse_ledger(_fixture_ledger())
    planned = _plan_retain(
        _check_selection([a], rows),
        _ctx("reviewed-scope-authority", "selftest evidence", live, states),
    )
    if not (len(planned) == 1 and planned[0].new_state == "retain"):
        failures.append("retain-one: expected exactly one retain transition")
    before = _fixture_ledger().splitlines()
    for item in planned:
        item.row[2], item.row[3], item.row[4], item.row[5] = (
            item.new_state,
            item.rationale,
            "batch-t",
            item.evidence,
        )
    after = [_LEDGER_HEADER] + ["\t".join(row) for row in rows]
    for old, new in zip(before[1:], after[1:], strict=True):
        if old.split("\t")[0] == a:
            if new.split("\t")[2] != "retain":
                failures.append("retain-one: selected row did not transition")
        elif old != new:
            failures.append(f"retain-one: unrelated row changed {old[:12]}")
    _header, rows = _parse_ledger(_fixture_ledger())
    planned = _plan_retain(
        _check_selection([a, b], rows),
        _ctx("tool-false-positive", "selftest evidence", live, states),
    )
    if {item.row[0] for item in planned} != {a, b}:
        failures.append("retain-many: expected transitions for exactly the requested rows")


def _retain_refusal(
    failures: list[str],
    case: tuple[str, str, ReviewContext],
) -> None:
    """Assert one retain selection is rejected for its stated reason."""
    label, selection, context = case
    _header, rows = _parse_ledger(_fixture_ledger())
    _refused(
        failures,
        label,
        _plan_retain,
        _check_selection([selection], rows),
        context,
    )


def _test_retain_refusals(failures: list[str], a: str, c: str, d: str, f: str) -> None:
    """Prove stale, drifted, mis-rationaled, and reviewed retains refuse."""
    live, states = _fixture_live(), _fixture_states()
    cases = (
        ("retain-stale", c, _ctx("reviewed-scope-authority", "selftest evidence", live, states)),
        (
            "retain-drifted-binding",
            f,
            _ctx("reviewed-scope-authority", "selftest evidence", live, states),
        ),
        ("retain-missing-rationale", a, _ctx("", "selftest evidence", live, states)),
        (
            "retain-missing-evidence",
            a,
            _ctx("reviewed-scope-authority", "  ", live, states),
        ),
        (
            "retain-unknown-rationale",
            a,
            _ctx("no-such-category", "selftest evidence", live, states),
        ),
        (
            "retain-wrong-state-rationale",
            a,
            _ctx("resolved-construct-retired", "selftest evidence", live, states),
        ),
        ("retain-reviewed", d, _ctx("reviewed-scope-authority", "selftest evidence", live, states)),
        ("retain-quoted-evidence", a, _ctx("reviewed-scope-authority", 'say "hi"', live, states)),
    )
    for label, selection, context in cases:
        _retain_refusal(failures, (label, selection, context))


def _retire_refusal(
    failures: list[str],
    case: tuple[str, str, ReviewContext, list[str]],
) -> None:
    """Assert one retire selection is rejected for its stated reason."""
    label, selection, context, successors = case
    _header, rows = _parse_ledger(_fixture_ledger())
    _refused(
        failures,
        label,
        _plan_retire,
        _check_selection([selection], rows),
        context,
        successors,
    )


def _test_retire(failures: list[str], a: str, b: str, c: str, e: str) -> None:
    """Prove stale retirement, supersede tokens, and live-row refusal."""
    live, states = _fixture_live(), _fixture_states()
    _header, rows = _parse_ledger(_fixture_ledger())
    planned = _plan_retire(
        _check_selection([c], rows),
        _ctx("resolved-construct-retired", "construct gone", live, states),
        [],
    )
    if not (len(planned) == 1 and planned[0].new_state == "resolved"):
        failures.append("retire-stale: expected exactly one resolve transition")
    _header, rows = _parse_ledger(_fixture_ledger())
    planned = _plan_retire(
        _check_selection([c], rows),
        _ctx("superseded-identity-rebinding", "rebound decision", live, states),
        [b],
    )
    if len(planned) != 1 or planned[0].new_state != "superseded":
        failures.append("retire-supersede: expected exactly one supersede transition")
    elif f"replaced-by:{b}" not in planned[0].evidence:
        failures.append("retire-supersede: replaced-by token missing from evidence")
    cases = (
        ("retire-live", a, "resolved-construct-retired", "construct gone", []),
        ("retire-self-successor", c, "superseded-identity-rebinding", "rebound decision", [c]),
        (
            "retire-dead-successor",
            c,
            "superseded-identity-rebinding",
            "rebound decision",
            ["9" * 64],
        ),
        ("retire-resolved", e, "resolved-construct-retired", "construct gone", []),
        ("retire-retain-rationale", c, "reviewed-scope-authority", "construct gone", []),
    )
    for label, selection, rationale, evidence, successors in cases:
        _retire_refusal(
            failures,
            (label, selection, _ctx(rationale, evidence, live, states), successors),
        )


def _test_selection(failures: list[str], a: str) -> None:
    """Prove unknown, duplicated, malformed, and empty selections refuse."""
    _header, rows = _parse_ledger(_fixture_ledger())
    _refused(failures, "unknown-id", _check_selection, ["0" * 64], rows)
    _header, rows = _parse_ledger(_fixture_ledger())
    _refused(failures, "duplicate-flags", _check_selection, [a, a], rows)
    _header, rows = _parse_ledger(_fixture_ledger())
    _refused(failures, "bad-hex-id", _check_selection, ["zzz"], rows)
    _header, rows = _parse_ledger(_fixture_ledger())
    _refused(failures, "empty-selection", _check_selection, [], rows)
    dup_text = _fixture_ledger() + f"{a}\t{'1' * 64}\tunreviewed\t\t\t\n"
    _header, dup_rows = _parse_ledger(dup_text)
    _refused(failures, "duplicate-rows", _check_selection, [a], dup_rows)


def _test_malformed(failures: list[str]) -> None:
    """Prove malformed ledgers fail closed before any selection runs."""
    bad = [
        "wrong\n" + "a" * 64 + "\t" + "1" * 64 + "\tunreviewed\t\t\n",
        _LEDGER_HEADER + "\n" + "a" * 64 + "\t" + "1" * 64 + "\tunreviewed\n",
        _LEDGER_HEADER + "\nzz\t" + "1" * 64 + "\tunreviewed\t\t\n",
        _LEDGER_HEADER + "\n" + "a" * 64 + "\t" + "1" * 64 + "\tnope\t\t\n",
        _LEDGER_HEADER + "\n" + "a" * 64 + "\t" + "1" * 64 + "\tunreviewed\t\tx\n",
    ]
    for index, text in enumerate(bad):
        _refused(failures, f"malformed-{index}", _parse_ledger, text)


def _test_batch(failures: list[str], a: str) -> None:
    """Prove batch-id collision refusal and the gate digest formula."""
    stamp = datetime.datetime(2024, 1, 2, 3, 4, 5, tzinfo=datetime.UTC)
    try:
        _new_batch_id({"selective-review-20240102-030405"}, stamp)
        failures.append("batch-collision: expected refusal")
    except ReviewError:
        pass
    batch_id = _new_batch_id(set(), stamp)
    if batch_id != "selective-review-20240102-030405":
        failures.append("batch-id-shape: unexpected minted id")
    member = [a, "1" * 64, "retain", "reviewed-scope-authority", batch_id, "selftest evidence"]
    payload = f"{a}\t{'1' * 64}\tretain\treviewed-scope-authority\tselftest evidence"
    if _batch_digest([member]) != hashlib.sha256(payload.encode("utf-8")).hexdigest():
        failures.append("batch-digest: formula drifted from the gate payload shape")


def _test_reconcile_preconditions(failures: list[str], a: str, b: str, c: str) -> None:
    """Prove planned rows satisfy the gate reconciler's preconditions.

    The gate approves a retain row only for a live site with an exact binding
    match, and closes a resolved row only for a vanished site; a superseded
    row additionally needs every replaced-by successor live. These checks
    mirror scripts/checks/suppression_ledger.py::_reconcile_row without
    importing gate code into the review tool.
    """

    def live_ok(site: str, binding: str, live: dict[str, str]) -> bool:
        return live.get(site) == binding

    live, states = _fixture_live(), _fixture_states()
    _header, rows = _parse_ledger(_fixture_ledger())
    planned = _plan_retain(
        _check_selection([a], rows),
        _ctx("reviewed-scope-authority", "selftest evidence", live, states),
    )
    item = planned[0]
    if item.new_state != "retain" or states[item.rationale] != "retain":
        failures.append("precondition-retain: state/rationale mismatch")
    if not live_ok(item.row[0], item.row[1], live):
        failures.append("precondition-retain: site not live with exact binding")
    _header, rows = _parse_ledger(_fixture_ledger())
    planned = _plan_retire(
        _check_selection([c], rows),
        _ctx("resolved-construct-retired", "construct gone", live, states),
        [],
    )
    item = planned[0]
    if item.new_state != "resolved" or item.row[0] in live:
        failures.append("precondition-resolve: site must be vanished")
    _header, rows = _parse_ledger(_fixture_ledger())
    planned = _plan_retire(
        _check_selection([c], rows),
        _ctx("superseded-identity-rebinding", "rebound decision", live, states),
        [b],
    )
    item = planned[0]
    if item.new_state != "superseded" or item.row[0] in live:
        failures.append("precondition-supersede: old site must be vanished")
    if f"replaced-by:{b}" not in item.evidence or b not in live:
        failures.append("precondition-supersede: successor must be live and named")


def _test_tsv_cells(failures: list[str], a: str, c: str) -> None:
    """Prove tab/newline/quote review fields refuse on both actions."""
    live, states = _fixture_live(), _fixture_states()
    for label, rationale, evidence in [
        ("tab-evidence", "reviewed-scope-authority", "audit\tprobe"),
        ("newline-evidence", "reviewed-scope-authority", "audit\nprobe"),
        ("quote-evidence", "reviewed-scope-authority", 'say "hi"'),
        ("tab-rationale", "reviewed\tscope", "selftest evidence"),
        ("newline-rationale", "reviewed\nscope", "selftest evidence"),
    ]:
        _header, rows = _parse_ledger(_fixture_ledger())
        _refused(
            failures,
            f"retain-{label}",
            _plan_retain,
            _check_selection([a], rows),
            _ctx(rationale, evidence, live, states),
        )
        _header, rows = _parse_ledger(_fixture_ledger())
        _refused(
            failures,
            f"retire-{label}",
            _plan_retire,
            _check_selection([c], rows),
            _ctx(rationale, evidence, live, states),
            [],
        )
    before = _fixture_ledger()
    _header, rows = _parse_ledger(before)
    with suppress(ReviewError):
        _plan_retain(
            _check_selection([a], rows),
            _ctx("reviewed-scope-authority", "audit\tprobe", live, states),
        )
    if [_LEDGER_HEADER] + ["\t".join(row) for row in rows] != before.splitlines():
        failures.append("refused-plan-untouched: failed validation mutated rows")


def _apply_like_run(planned: list[PlannedTransition], batch: str) -> None:
    """Mutate rows exactly as run_selective does after successful planning."""
    for item in planned:
        item.row[2], item.row[3], item.row[4], item.row[5] = (
            item.new_state,
            item.rationale,
            batch,
            item.evidence,
        )


def _test_write_reporting(failures: list[str], a: str, b: str, c: str) -> None:
    """Prove real writes report exact old states without changing outcomes."""
    live, states = _fixture_live(), _fixture_states()
    specs = [
        ("retain", "reviewed-scope-authority", "selftest evidence", [], "unreviewed -> retain"),
        ("resolved", "resolved-construct-retired", "construct gone", [], "unreviewed -> resolved"),
    ]
    for want_new, rationale, evidence, replaced, want_line in specs:
        _header, rows = _parse_ledger(_fixture_ledger())
        before_ids = [row[0] for row in rows]
        target = a if want_new == "retain" else c
        ctx = _ctx(rationale, evidence, live, states)
        if want_new == "retain":
            planned = _plan_retain(_check_selection([target], rows), ctx)
        else:
            planned = _plan_retire(_check_selection([target], rows), ctx, replaced)
        _apply_like_run(planned, "batch-t")
        lines = _describe(planned, "batch-t")
        if len(lines) != 1 or want_line not in lines[0]:
            failures.append(f"write-report-{want_new}: expected exact {want_line!r}")
        if [row[0] for row in rows] != before_ids:
            failures.append(f"write-report-{want_new}: row ordering changed")
        members = [row for row in rows if row[0] == target]
        payload = "\n".join(f"{row[0]}\t{row[1]}\t{row[2]}\t{row[3]}\t{row[5]}" for row in members)
        if _batch_digest(members) != hashlib.sha256(payload.encode("utf-8")).hexdigest():
            failures.append(f"write-report-{want_new}: batch digest changed")
        fixture = _fixture_ledger()
        failures.extend(
            f"write-report-{want_new}: unrelated row changed"
            for row in rows
            if row[0] != target and "\t".join(row) not in fixture
        )
    _header, rows = _parse_ledger(_fixture_ledger())
    planned = _plan_retire(
        _check_selection([c], rows),
        _ctx("superseded-identity-rebinding", "rebound decision", live, states),
        [b],
    )
    _apply_like_run(planned, "batch-t")
    lines = _describe(planned, "batch-t")
    if len(lines) != 1 or "unreviewed -> superseded" not in lines[0]:
        failures.append("write-report-superseded: expected exact old state")
    if f"replaced-by:{b}" not in planned[0].evidence:
        failures.append("write-report-superseded: successor token missing")


def _test_dry_run(failures: list[str], a: str, b: str) -> None:
    live, states = _fixture_live(), _fixture_states()
    before_text = _fixture_ledger()
    _header, rows = _parse_ledger(before_text)
    planned = _plan_retain(
        _check_selection([a, b], rows),
        _ctx("reviewed-scope-authority", "selftest evidence", live, states),
    )
    out = "\n".join(f"  {line}" for line in _describe(planned, "batch-dry"))
    if a not in out or b not in out or "unreviewed -> retain" not in out:
        failures.append("dry-run-names-rows: output does not identify transitions")
    current = [_LEDGER_HEADER] + ["\t".join(row) for row in rows]
    if current != before_text.splitlines():
        failures.append("dry-run-ledger-untouched: planning mutated rows")


def run_selftest() -> int:
    """Prove selective retain/retire guards both ways on synthetic fixtures."""
    failures: list[str] = []
    a, b, c, d, e, f = ("a" * 64, "b" * 64, "c" * 64, "d" * 64, "e" * 64, "f" * 64)
    _test_retain(failures, a, b)
    _test_retain_refusals(failures, a, c, d, f)
    _test_retire(failures, a, b, c, e)
    _test_selection(failures, a)
    _test_malformed(failures)
    _test_batch(failures, a)
    _test_reconcile_preconditions(failures, a, b, c)
    _test_tsv_cells(failures, a, c)
    _test_write_reporting(failures, a, b, c)
    _test_dry_run(failures, a, b)
    if failures:
        print("review_suppressions.py --selftest: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print(
        "review_suppressions.py --selftest: PASS "
        "(selective retain/retire, fail-closed guards, reconciler preconditions, dry-run)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
