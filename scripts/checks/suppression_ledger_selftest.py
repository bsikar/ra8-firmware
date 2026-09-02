# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Both-direction fixtures for the fail-closed review ledger."""

from __future__ import annotations

import hashlib
from pathlib import Path

from selftest_assert import expect
from suppression_identity import IDENTITY_SCHEMA_VERSION
from suppression_ledger import (
    BATCHES_PATH,
    LEDGER_PATH,
    RATIONALES_PATH,
    apply_ledger,
    candidate_rows,
    load_batches,
    load_rationales,
)
from suppression_model import Inventory, Suppression
from suppression_scan import scan_paths

_RATIONALES = """schema: suppression-review-rationales-1
categories:
  bounded-cleanup-status-mask:
    state: retain
    applicability: enumerated benign failure with an immediate consumer
    evidence: [consumer-or-assert]
    revalidate: when the consumer changes
  portable-test-prerequisite-boundary:
    state: retain
    applicability: direct portable skip, registered gate failure
    evidence: [test-name, passing-counterpart, registered-gate]
    revalidate: when the test or gate changes
  vendor-upstream-preserved:
    state: retain
    applicability: vendored SOUP bound to the exact blob
    evidence: [upstream-pin]
    revalidate: on blob change
  fix-remove:
    state: fix-required
    applicability: delete the marker
    evidence: [review-decision]
    revalidate: never
  superseded-identity-rebinding:
    state: superseded
    applicability: the decision moves to the live replacement site named here
    evidence: [replacement-site]
    revalidate: never
  resolved-construct-retired:
    state: resolved
    applicability: the construct is gone and nothing live replaces it
    evidence: [removal-evidence]
    revalidate: never
"""
_FILES = {
    "tool.sh": "#!/bin/sh\nprobe || true  # enumerated benign probe\n",
    "libs/third_party/vendor/legacy.txt": "",
}


def _batch_digest(rows: list[str]) -> str:
    """Recompute the recorded digest for one batch's ordered rows."""
    payload = "\n".join(
        "\t".join(
            (
                line.split("\t")[0],
                line.split("\t")[1],
                line.split("\t")[2],
                line.split("\t")[3],
                line.split("\t")[5],
            )
        )
        for line in rows
    ).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def _write_ledger(root: Path, rows: list[str], *, schema: str = IDENTITY_SCHEMA_VERSION) -> None:
    """Write the three committed ledger authorities for one fixture run."""
    (root / ".github").mkdir(exist_ok=True)
    (root / RATIONALES_PATH).write_text(_RATIONALES, encoding="ascii")
    header = "site_id\tbinding_sha256\tstate\trationale_id\tbatch_id\tevidence_ref"
    (root / LEDGER_PATH).write_text("\n".join([header, *rows]) + "\n", encoding="ascii")
    reviewed = [row for row in rows if row.split("\t")[2] != "unreviewed"]
    (root / BATCHES_PATH).write_text(
        "schema: suppression-review-batches-1\n"
        "batches:\n"
        "  - id: batch-fixture\n"
        "    authority: repository-suppression-review\n"
        "    date: 2026-08-24\n"
        f"    identity_schema: {schema}\n"
        f"    assigned_rows: {len(reviewed)}\n"
        f"    rows_sha256: {_batch_digest(reviewed)}\n",
        encoding="ascii",
    )


def _scan(root: Path, files: dict[str, str]) -> Inventory:
    """Materialize fixture files and scan them without repository floors."""
    for rel, text in files.items():
        target = root / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        if rel.endswith("legacy.txt"):
            target.write_bytes(b"vendored-\xff-text\n")
        else:
            target.write_text(text, encoding="ascii")
    return scan_paths(root, sorted(files))


def _codes(inventory: Inventory) -> set[str]:
    """Return the ledger-relevant finding codes of one reconciliation."""
    return {item.code for item in inventory.findings}


def _shell_row(inventory: Inventory) -> Suppression:
    """Return the shell-status fixture row."""
    return next(item for item in inventory.suppressions if item.family == "shell-status")


def _vendor_row(inventory: Inventory) -> Suppression:
    """Return the vendored encoding-exemption fixture row."""
    return next(item for item in inventory.suppressions if item.family == "encoding-exemption")


def _retain(row: Suppression, rationale: str = "bounded-cleanup-status-mask") -> str:
    """Render one retained ledger row for a live inventory row."""
    return (
        f"{row.site_id}\t{row.binding_sha256}\tretain\t{rationale}\tbatch-fixture\tevidence:fixture"
    )


def _retain_rows(inventory: Inventory) -> list[str]:
    """Render a retain row for every live suppression the fixture scanned."""
    return [
        _retain(item, "vendor-upstream-preserved")
        if item.family == "encoding-exemption"
        else _retain(item)
        for item in inventory.suppressions
    ]


def _closed(site: str, state: str, rationale: str, evidence: str) -> str:
    """Render one closed ledger row for a site the inventory no longer carries."""
    return f"{site}\t{'1' * 64}\t{state}\t{rationale}\tbatch-fixture\t{evidence}"


def _write_portable_contract(root: Path, *, fail_closed: bool) -> None:
    """Write the exact test and gate evidence used by portable-skip fixtures."""
    test = root / "tools/work/tests/test_portable.py"
    test.parent.mkdir(parents=True, exist_ok=True)
    test.write_text(
        "def test_direct_skip():\n    pass\n\ndef test_registered_failure():\n    pass\n",
        encoding="ascii",
    )
    helper = root / "tools/work/tests/fixtures/work_testlib.py"
    helper.parent.mkdir(parents=True, exist_ok=True)
    helper.write_text(
        'import os\nREGISTERED_GATE_ENV = "RA8_WORK_HARNESS_REGISTERED_GATE"\n'
        'if os.environ.get(REGISTERED_GATE_ENV) == "1":\n'
        '    message = "missing"\n    raise RuntimeError(message)\n',
        encoding="ascii",
    )
    gate = root / "scripts/ci/gates/tests.sh"
    gate.parent.mkdir(parents=True, exist_ok=True)
    registration = (
        "  RA8_WORK_HARNESS_REGISTERED_GATE=1 python3 -I tools/work/src/work.py --selftest\n"
        if fail_closed
        else "  python3 -I tools/work/src/work.py --selftest\n"
    )
    gate.write_text(
        "gate_work_harness() (\n  require_cmd bash\n  require_cmd sh\n" + registration + ")\n",
        encoding="ascii",
    )


def _portable_evidence(gate: str = "work-harness") -> str:
    """Return a complete portable-prerequisite evidence reference."""
    return (
        "test-name:tools/work/tests/test_portable.py::test_direct_skip "
        "passing-counterpart:tools/work/tests/test_portable.py::test_registered_failure "
        f"registered-gate:{gate}"
    )


def _closing_codes() -> set[str]:
    """Return every reconciliation code a well-formed closed row must avoid."""
    return {
        "ledger-unknown-reference",
        "ledger-missing-site",
        "ledger-stale-site",
        "ledger-state-conflict",
        "ledger-resolved-still-present",
    }


def assert_ledger_gate(base: Path, failures: list[str]) -> None:
    """Assert every ledger rejection fires and a complete ledger passes."""
    root = base / "ledger-repo"
    root.mkdir()
    inventory = _scan(root, _FILES)
    shell = _shell_row(inventory)
    vendor = _vendor_row(inventory)
    others = [
        item
        for item in inventory.suppressions
        if item.site_id not in {shell.site_id, vendor.site_id}
    ]
    complete = sorted(
        [_retain(shell), _retain(vendor, "vendor-upstream-preserved")]
        + [_retain(item) for item in others]
    )
    _write_ledger(root, complete)
    apply_ledger(inventory, root)
    approved = sum(item.disposition == "approved" for item in inventory.suppressions)
    expect(
        approved == len(inventory.suppressions)
        and not (_codes(inventory) & {"ledger-missing-site", "ledger-binding-mismatch"}),
        "quiet: a complete matching ledger approves every retained site",
        failures,
    )

    fresh = _scan(base / "ledger-missing", _FILES)
    ledger_without_shell = sorted(
        [_retain(vendor, "vendor-upstream-preserved")] + [_retain(item) for item in others]
    )
    _write_ledger(base / "ledger-missing", ledger_without_shell)
    apply_ledger(fresh, base / "ledger-missing")
    expect(
        "ledger-missing-site" in _codes(fresh),
        "must fire: a live suppression absent from the ledger fails",
        failures,
    )
    _assert_ledger_binding_drift(base, complete, failures)
    stale_root = base / "ledger-stale"
    stale = _scan(stale_root, {"tool.sh": _FILES["tool.sh"]})
    stale_rows = sorted(
        [_retain(item) for item in stale.suppressions]
        + [_retain(vendor, "vendor-upstream-preserved")]
    )
    _write_ledger(stale_root, stale_rows)
    apply_ledger(stale, stale_root)
    expect(
        "ledger-stale-site" in _codes(stale),
        "must fire: a retained ledger row without a live site fails",
        failures,
    )

    _assert_ledger_resolution(base, failures)
    _assert_ledger_supersession(base, failures)
    _assert_portable_prerequisite_evidence(base, failures)
    _assert_committed_vocabulary(base, failures)
    _assert_ledger_shape_rules(base, failures)


def _portable_case(base: Path, name: str, evidence: str, *, fail_closed: bool = True) -> set[str]:
    """Apply one portable-prerequisite ledger fixture and return finding codes."""
    root = base / f"ledger-portable-{name}"
    inventory = _scan(root, _FILES)
    _write_portable_contract(root, fail_closed=fail_closed)
    shell = _shell_row(inventory)
    rows = [
        _retain(item, "vendor-upstream-preserved")
        if item.family == "encoding-exemption"
        else (
            f"{item.site_id}\t{item.binding_sha256}\tretain\t"
            "portable-test-prerequisite-boundary\tbatch-fixture\t"
            f"{evidence}"
            if item.site_id == shell.site_id
            else _retain(item)
        )
        for item in inventory.suppressions
    ]
    _write_ledger(root, sorted(rows))
    apply_ledger(inventory, root)
    return _codes(inventory)


def _assert_portable_prerequisite_evidence(base: Path, failures: list[str]) -> None:
    """Assert portable skips require exact live tests and a fail-closed gate."""
    accepted = _portable_case(base, "accepted", _portable_evidence())
    expect(
        not (
            accepted
            & {
                "malformed-review-ledger",
                "ledger-unknown-reference",
                "ledger-invalid-gate-contract",
            }
        ),
        "quiet: exact tests plus a fail-closed registered gate justify a portable skip",
        failures,
    )
    missing = _portable_case(
        base,
        "missing",
        "test-name:tools/work/tests/test_portable.py::test_direct_skip "
        "registered-gate:work-harness",
    )
    expect(
        "malformed-review-ledger" in missing,
        "must fire: portable evidence missing a required field is rejected",
        failures,
    )
    unknown_test = _portable_case(
        base,
        "unknown-test",
        "test-name:tools/work/tests/test_portable.py::test_absent "
        "passing-counterpart:tools/work/tests/test_portable.py::test_registered_failure "
        "registered-gate:work-harness",
    )
    expect(
        "ledger-unknown-reference" in unknown_test,
        "must fire: portable evidence naming an unknown test is rejected",
        failures,
    )
    unknown_gate = _portable_case(base, "unknown-gate", _portable_evidence("ghost-gate"))
    expect(
        "ledger-unknown-reference" in unknown_gate,
        "must fire: portable evidence naming an unknown gate is rejected",
        failures,
    )
    skippable = _portable_case(base, "skippable-gate", _portable_evidence(), fail_closed=False)
    expect(
        "ledger-invalid-gate-contract" in skippable,
        "must fire: a registered gate that can skip its prerequisite is rejected",
        failures,
    )


def _assert_ledger_supersession(base: Path, failures: list[str]) -> None:
    """Assert supersession closes a row only via a live, ledgered successor."""
    probe = _scan(base / "ledger-superseded", _FILES)
    expect(
        {"shell-status", "encoding-exemption"} <= {item.family for item in probe.suppressions},
        "quiet: the supersession fixture scans a non-empty inventory",
        failures,
    )
    gone = "0" * 64
    for rationale in ("fix-remove", "superseded-identity-rebinding"):
        root = base / f"ledger-sup-{rationale}"
        inventory = _scan(root, _FILES)
        successor = _shell_row(inventory)
        rows = sorted(
            [
                *_retain_rows(inventory),
                _closed(gone, "superseded", rationale, f"replaced-by:{successor.site_id}"),
            ]
        )
        _write_ledger(root, rows)
        apply_ledger(inventory, root)
        expect(
            not (_codes(inventory) & _closing_codes()),
            f"quiet: a superseded row under {rationale} naming a live successor passes",
            failures,
        )
    _assert_supersession_references(base, failures)


def _assert_supersession_references(base: Path, failures: list[str]) -> None:
    """Assert every unusable ``replaced-by:`` reference fails closed."""
    gone = "0" * 64
    cases = {
        "no successor named": "evidence:removed",
        "malformed successor token": "replaced-by:not-a-site",
        "successor that is not live": f"replaced-by:{'2' * 64}",
    }
    for label, evidence in cases.items():
        root = base / f"ledger-sup-{label.split()[0]}"
        inventory = _scan(root, _FILES)
        rows = sorted(
            [*_retain_rows(inventory), _closed(gone, "superseded", "fix-remove", evidence)]
        )
        _write_ledger(root, rows)
        apply_ledger(inventory, root)
        expect(
            "ledger-unknown-reference" in _codes(inventory),
            f"must fire: a superseded row with {label} is rejected",
            failures,
        )
    _assert_supersession_boundaries(base, failures)


def _assert_supersession_boundaries(base: Path, failures: list[str]) -> None:
    """Assert an unledgered successor and a still-live site both fail closed."""
    gone = "0" * 64
    missing_root = base / "ledger-sup-unledgered"
    missing = _scan(missing_root, _FILES)
    successor = _shell_row(missing)
    _write_ledger(
        missing_root,
        [_closed(gone, "superseded", "fix-remove", f"replaced-by:{successor.site_id}")],
    )
    apply_ledger(missing, missing_root)
    expect(
        "ledger-missing-site" in _codes(missing),
        "must fire: a successor site carrying no ledger row of its own is rejected",
        failures,
    )

    present_root = base / "ledger-sup-present"
    present = _scan(present_root, _FILES)
    shell = _shell_row(present)
    vendor = _vendor_row(present)
    still_live = (
        f"{shell.site_id}\t{shell.binding_sha256}\tsuperseded\tfix-remove"
        f"\tbatch-fixture\treplaced-by:{vendor.site_id}"
    )
    _write_ledger(present_root, sorted([still_live, _retain(vendor, "vendor-upstream-preserved")]))
    apply_ledger(present, present_root)
    expect(
        "ledger-resolved-still-present" in _codes(present),
        "must fire: a superseded row whose own site is still live is rejected",
        failures,
    )
    _assert_supersession_vocabulary(base, failures)


def _assert_supersession_vocabulary(base: Path, failures: list[str]) -> None:
    """Assert the retain / closing vocabulary boundary holds in both directions."""
    root = base / "ledger-sup-conflict"
    inventory = _scan(root, _FILES)
    successor = _shell_row(inventory)
    named = f"replaced-by:{successor.site_id}"
    rows = sorted(
        [
            *_retain_rows(inventory),
            _closed("0" * 64, "superseded", "bounded-cleanup-status-mask", named),
        ]
    )
    _write_ledger(root, rows)
    apply_ledger(inventory, root)
    expect(
        "ledger-state-conflict" in _codes(inventory),
        "must fire: a retain-vocabulary rationale in state superseded is rejected",
        failures,
    )

    retired_root = base / "ledger-sup-retired"
    retired = _scan(retired_root, _FILES)
    retired_rows = sorted(
        [
            *_retain_rows(retired),
            _closed("0" * 64, "resolved", "resolved-construct-retired", "evidence:removed"),
        ]
    )
    _write_ledger(retired_root, retired_rows)
    apply_ledger(retired, retired_root)
    expect(
        not (_codes(retired) & _closing_codes()),
        "quiet: a resolved row under resolved-construct-retired needs no successor",
        failures,
    )


def _assert_committed_vocabulary(base: Path, failures: list[str]) -> None:
    """Assert the committed vocabulary parses and offers both closing states."""
    categories, findings = load_rationales(Path(__file__).resolve().parents[2])
    states = {spec["state"] for spec in categories.values()}
    expect(
        not findings and {"superseded", "resolved"} <= states,
        "quiet: the committed vocabulary parses and offers both closing states",
        failures,
    )
    root = base / "ledger-vocabulary"
    (root / ".github").mkdir(parents=True)
    (root / RATIONALES_PATH).write_text(
        "schema: suppression-review-rationales-1\n"
        "categories:\n"
        "  outside-the-vocabulary:\n"
        "    state: approved\n"
        "    applicability: a state no ledger row may carry\n"
        "    evidence: [review-decision]\n",
        encoding="ascii",
    )
    _, rejected = load_rationales(root)
    expect(
        any(item.code == "malformed-review-ledger" for item in rejected),
        "must fire: a category naming a state outside the ledger vocabulary is rejected",
        failures,
    )


def _assert_ledger_binding_drift(base: Path, complete: list[str], failures: list[str]) -> None:
    """Assert reason and vendored-blob edits reopen review."""
    drift_root = base / "ledger-drift"
    drift = _scan(
        drift_root,
        {**_FILES, "tool.sh": "#!/bin/sh\nprobe || true  # a different rationale\n"},
    )
    _write_ledger(drift_root, complete)
    apply_ledger(drift, drift_root)
    drift_codes = _codes(drift)
    expect(
        "ledger-binding-mismatch" in drift_codes
        and all(
            item.disposition != "approved"
            for item in drift.suppressions
            if item.family == "shell-status"
        ),
        "must fire: a reason edit preserves the site but invalidates its binding",
        failures,
    )

    vendor_root = base / "ledger-vendor"
    vendor_changed = _scan(vendor_root, _FILES)
    (vendor_root / "libs/third_party/vendor/legacy.txt").write_bytes(b"vendored-\xfe-changed\n")
    vendor_changed = scan_paths(vendor_root, sorted(_FILES))
    _write_ledger(vendor_root, complete)
    apply_ledger(vendor_changed, vendor_root)
    expect(
        "ledger-binding-mismatch" in _codes(vendor_changed),
        "must fire: a vendored blob change invalidates its encoding review",
        failures,
    )


def _assert_ledger_resolution(base: Path, failures: list[str]) -> None:
    """Assert resolved rows demand absence and re-appearance fails."""
    resolved_root = base / "ledger-resolved"
    resolved = _scan(resolved_root, {"tool.sh": _FILES["tool.sh"]})
    gone = f"{'0' * 64}\t{'1' * 64}\tresolved\tfix-remove\tbatch-fixture\tevidence:removed"
    resolved_rows = sorted([_retain(item) for item in resolved.suppressions] + [gone])
    _write_ledger(resolved_root, resolved_rows)
    apply_ledger(resolved, resolved_root)
    expect(
        "ledger-resolved-still-present" not in _codes(resolved)
        and "ledger-stale-site" not in _codes(resolved),
        "quiet: a resolved row whose site is gone is the completed shape",
        failures,
    )
    still_root = base / "ledger-still-present"
    still = _scan(still_root, {"tool.sh": _FILES["tool.sh"]})
    live = next(iter(still.suppressions))
    present = (
        f"{live.site_id}\t{live.binding_sha256}\tresolved"
        "\tfix-remove\tbatch-fixture\tevidence:removed"
    )
    still_rows = sorted(
        [present] + [_retain(item) for item in still.suppressions if item.site_id != live.site_id]
    )
    _write_ledger(still_root, still_rows)
    apply_ledger(still, still_root)
    expect(
        "ledger-resolved-still-present" in _codes(still),
        "must fire: a resolved site that still exists fails",
        failures,
    )
    conflict_root = base / "ledger-resolved-conflict"
    conflict = _scan(conflict_root, {"tool.sh": _FILES["tool.sh"]})
    conflict_rows = sorted(
        [
            *(_retain(item) for item in conflict.suppressions),
            _closed("0" * 64, "resolved", "bounded-cleanup-status-mask", "evidence:removed"),
        ]
    )
    _write_ledger(conflict_root, conflict_rows)
    apply_ledger(conflict, conflict_root)
    expect(
        "ledger-state-conflict" in _codes(conflict),
        "must fire: a retain-vocabulary rationale in state resolved is rejected",
        failures,
    )


def _assert_ledger_shape_rules(base: Path, failures: list[str]) -> None:
    """Assert structural, batch, and state rules fail closed."""
    root = base / "ledger-shape"
    inventory = _scan(root, {"tool.sh": _FILES["tool.sh"]})
    row = next(iter(inventory.suppressions))

    fix_rows = [
        f"{row.site_id}\t{row.binding_sha256}\tfix-required\tfix-remove\tbatch-fixture\tevidence:fixture"
    ]
    _write_ledger(root, fix_rows)
    apply_ledger(inventory, root)
    expect(
        "ledger-fix-required" in _codes(inventory)
        and all(item.disposition != "approved" for item in inventory.suppressions),
        "must fire: an active fix-required row keeps the gate red",
        failures,
    )

    cases = {
        "unknown rationale": (
            f"{row.site_id}\t{row.binding_sha256}\tretain\tno-such-category\tbatch-fixture\tevidence:x",
            "ledger-unknown-reference",
        ),
        "state conflict": (
            f"{row.site_id}\t{row.binding_sha256}\tretain\tfix-remove\tbatch-fixture\tevidence:x",
            "ledger-state-conflict",
        ),
        "blank evidence": (
            f"{row.site_id}\t{row.binding_sha256}\tretain\tbounded-cleanup-status-mask\tbatch-fixture\t",
            "malformed-review-ledger",
        ),
        "unknown state": (
            f"{row.site_id}\t{row.binding_sha256}\tapproved\tbounded-cleanup-status-mask\tbatch-fixture\tevidence:x",
            "malformed-review-ledger",
        ),
        "unknown batch": (
            f"{row.site_id}\t{row.binding_sha256}\tretain\tbounded-cleanup-status-mask\tbatch-ghost\tevidence:x",
            "ledger-unknown-reference",
        ),
    }
    for label, (ledger_row, code) in cases.items():
        fresh = _scan(
            base / f"ledger-{code}-{label[:7].replace(' ', '')}", {"tool.sh": _FILES["tool.sh"]}
        )
        fixture_root = base / f"ledger-{code}-{label[:7].replace(' ', '')}"
        _write_ledger(fixture_root, [ledger_row])
        apply_ledger(fresh, fixture_root)
        expect(code in _codes(fresh), f"must fire: {label} is rejected", failures)
    _assert_ledger_duplicate_rules(base, failures)


def _assert_ledger_duplicate_rules(base: Path, failures: list[str]) -> None:
    """Assert duplicate and unsorted ledger rows fail closed."""
    dup_root = base / "ledger-dup"
    dup = _scan(dup_root, {"tool.sh": _FILES["tool.sh"]})
    dup_row = _retain(next(iter(dup.suppressions)))
    _write_ledger(dup_root, [dup_row, dup_row])
    apply_ledger(dup, dup_root)
    expect("ledger-duplicate-site" in _codes(dup), "must fire: duplicate site ids fail", failures)

    unsorted_root = base / "ledger-unsorted"
    unsorted = _scan(unsorted_root, _FILES)
    shell = _shell_row(unsorted)
    vendor = _vendor_row(unsorted)
    pair = sorted([_retain(shell), _retain(vendor, "vendor-upstream-preserved")], reverse=True)
    if pair[0].split("\t")[0] > pair[1].split("\t")[0]:
        _write_ledger(unsorted_root, pair)
        apply_ledger(unsorted, unsorted_root)
        expect(
            "malformed-review-ledger" in _codes(unsorted),
            "must fire: unsorted ledger rows fail",
            failures,
        )

    _assert_ledger_batch_rules(base, failures)


def _assert_ledger_batch_rules(base: Path, failures: list[str]) -> None:
    """Assert schema, batch, and bootstrap-candidate rules fail closed."""
    schema_root = base / "ledger-schema"
    schema = _scan(schema_root, {"tool.sh": _FILES["tool.sh"]})
    schema_rows = [_retain(item) for item in schema.suppressions]
    _write_ledger(schema_root, sorted(schema_rows), schema="1-old-schema")
    apply_ledger(schema, schema_root)
    expect(
        "ledger-schema-mismatch" in _codes(schema),
        "must fire: an identity-schema change fails closed",
        failures,
    )

    batch_root = base / "ledger-batchcount"
    batch = _scan(batch_root, {"tool.sh": _FILES["tool.sh"]})
    batch_rows = sorted(_retain(item) for item in batch.suppressions)
    _write_ledger(batch_root, batch_rows)
    text = (batch_root / BATCHES_PATH).read_text(encoding="ascii")
    (batch_root / BATCHES_PATH).write_text(
        text.replace(f"assigned_rows: {len(batch_rows)}", f"assigned_rows: {len(batch_rows) + 1}"),
        encoding="ascii",
    )
    apply_ledger(batch, batch_root)
    expect(
        "ledger-batch-mismatch" in _codes(batch),
        "must fire: a batch row-count mismatch fails",
        failures,
    )

    candidates = candidate_rows(batch)
    expect(
        bool(candidates) and all("\tunreviewed\t" in line for line in candidates),
        "quiet: generated candidates are always unreviewed, never approvals",
        failures,
    )
    unrev_root = base / "ledger-unrev"
    unrev = _scan(unrev_root, {"tool.sh": _FILES["tool.sh"]})
    unrev_rows = sorted(candidate_rows(unrev))
    _write_ledger(unrev_root, unrev_rows)
    apply_ledger(unrev, unrev_root)
    expect(
        "ledger-unreviewed" in _codes(unrev)
        and all(item.disposition != "approved" for item in unrev.suppressions),
        "must fire: bootstrap candidates keep the gate red until reviewed",
        failures,
    )
    _assert_batch_identity_rules(base, failures)


def _batch_documents() -> tuple[dict[str, tuple[str, str]], str, str]:
    """Build the rejected, valid, and merge-key batch authorities as YAML text."""
    header = "schema: suppression-review-batches-1\nbatches:\n"
    tail = (
        "    authority: repository-suppression-review\n"
        "    date: 2026-08-24\n"
        f"    identity_schema: {IDENTITY_SCHEMA_VERSION}\n"
        "    assigned_rows: 0\n"
        f"    rows_sha256: {'0' * 64}\n"
    )
    rejected = {
        "duplicate batch identity": (
            header + "  - id: batch-one\n" + tail + "  - id: batch-one\n" + tail,
            "ledger-duplicate-batch",
        ),
        "duplicate key inside a record": (
            header + "  - id: batch-one\n    id: batch-two\n" + tail,
            "malformed-review-ledger",
        ),
        "duplicate key at document top level": (
            header + "  - id: batch-one\n" + tail + "batches:\n  - id: batch-two\n" + tail,
            "malformed-review-ledger",
        ),
    }
    valid = header + "  - id: batch-one\n" + tail + "  - id: batch-two\n" + tail
    merged = (
        "schema: suppression-review-batches-1\n"
        "shared: &shared\n"
        "  authority: inherited-authority\n"
        "  date: 2026-08-24\n"
        "batches:\n"
        "  - <<: *shared\n"
        "    authority: repository-suppression-review\n"
        "    id: batch-merge\n"
        f"    identity_schema: {IDENTITY_SCHEMA_VERSION}\n"
        "    assigned_rows: 0\n"
        f"    rows_sha256: {'0' * 64}\n"
    )
    return rejected, valid, merged


def _load_batches_document(root: Path, document: str) -> tuple[dict[str, dict], set[str]]:
    """Write one batches authority into a fixture root and load it."""
    (root / ".github").mkdir(parents=True, exist_ok=True)
    (root / BATCHES_PATH).write_text(document, encoding="ascii")
    batches, findings = load_batches(root)
    return batches, {item.code for item in findings}


def _assert_batch_identity_rules(base: Path, failures: list[str]) -> None:
    """Assert repeated batch identities fail and valid YAML still parses.

    PyYAML keeps the LAST value bound to a repeated mapping key and reports
    nothing, so a second ``id:`` inside a record, or a second ``batches:``
    block, silently replaced a reviewed authority before any application
    validation ran. Both directions are asserted here: every repeat is
    refused, and anchors, merge keys, and distinct identities still load
    exactly as written.
    """
    rejected, valid, merged = _batch_documents()
    for index, (label, (document, code)) in enumerate(rejected.items()):
        root = base / f"ledger-batchid-{index}"
        root.mkdir(parents=True, exist_ok=True)
        _batches, codes = _load_batches_document(root, document)
        expect(code in codes, f"must fire: {label} is rejected", failures)

    valid_root = base / "ledger-batchid-valid"
    valid_root.mkdir(parents=True, exist_ok=True)
    batches, codes = _load_batches_document(valid_root, valid)
    expect(
        not codes and sorted(batches) == ["batch-one", "batch-two"],
        "quiet: two distinct batch identities load unchanged",
        failures,
    )

    merge_root = base / "ledger-batchid-merge"
    merge_root.mkdir(parents=True, exist_ok=True)
    records, codes = _load_batches_document(merge_root, merged)
    expect(
        not codes
        and sorted(records) == ["batch-merge"]
        and records["batch-merge"]["authority"] == "repository-suppression-review"
        and str(records["batch-merge"]["date"]) == "2026-08-24",
        "quiet: a merge key and its explicit override keep documented YAML semantics",
        failures,
    )
