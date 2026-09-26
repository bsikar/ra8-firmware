# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Regression selftests for a SUCCESSFUL pass claiming capacity it never put back (#888).

``capacity-restore`` converges live admission to the host's CURRENT window
target and prints which one it used, and ``restore_admission`` reads it: both
RECOVERY arms refuse to call a reopen to ZERO instances a recovery, so the host
stays recorded at zero rather than reported as serving.  An ordinary apply is
deliberately NOT failed by that target, because converging the declaration is
what the transaction claims and a quiet-hours window of zero instances is the
operator's own policy.

The pass that SUCCEEDS then threw the number away.  ``record_success`` published
the receipt and cleared the host's stranded-at-zero record as "reconciled and
serving again", so a record a real earlier drain had written was dropped by a
pass whose own restore had just printed zero instances.  The next pass found a
fresh ``full_applied_at`` receipt with nothing due, said nothing about the host,
and exited 0: a fleet still DECLARED for its runners while serving none of
them, for as long as the window lasted and with nothing to escalate off.  That
is issue #888's own dry-run evidence, reached through the happy path instead of
a failure, which is why every earlier fix on the failure and exit paths left it
standing.

These tests pin the success path to the capacity it actually left in service:

* a reconciled host whose restore reported ZERO instances keeps its record and
  ages it, so it escalates at ``STRANDED_ESCALATION_PASSES`` like any host held
  at zero;
* a host with NO record is given none, because a declared quiet-hours target is
  not this pass draining anything: that would forge the proof
  ``frozen_image_release``, ``consumers_released`` and the drain budget read
  back off the record, and the pin that an apply is not failed by policy stands;
* a restore that reported real capacity still clears the record exactly as
  before;
* a restore that reported no target at all claims nothing either way, so an
  older host-local copy of the capacity script behaves as it did;
* the ARC arm reads its own restore too, which is the arm that never asked;
* the transaction reader answers to the restore's own output and to nothing else.
"""

from __future__ import annotations

import tempfile
from collections.abc import Sequence
from pathlib import Path
from types import ModuleType
from typing import Any

import fleet_reconcile_process as frp

FIRST_PASS = 3000
FULL_INTERVAL = 100
PRODUCER_INTERVAL = 50
DIGEST = "c" * 64
DECLARED_RUNNERS = 3
AGED_PASSES = 2
PRODUCER = "producer"
CONSUMER = "consumer"
ZERO_TARGET = "2026-09-17T23:05:11Z restoring current window target 0\n"
FULL_TARGET = f"2026-09-17T11:05:11Z restoring current window target {DECLARED_RUNNERS}\n"
SILENT_RESTORE = "2026-09-17T11:05:11Z restored\n"

VERBS = {
    "capacity-quarantine": "quarantine",
    "capacity-restore": "restore",
    "reconcile-parked-apply": "parked-apply",
    "reconcile-parked-check": "parked-check",
    "reconcile-activate": "activate",
    "reconcile-activation-check": "activation-check",
}


def _identity(argv: Sequence[str]) -> tuple[str, str]:
    """Return the fleet verb and host from one generated command."""
    return VERBS.get(argv[2], argv[2]), argv[-1]


def _data(*, arc_consumer: bool = False) -> dict[str, Any]:
    """Return one image producer and one capacity-managed consumer."""
    return {
        "runner_image": {"source_host": PRODUCER},
        "hosts": {
            PRODUCER: {
                "class": "docker_linux",
                "runners": {"instances": DECLARED_RUNNERS},
                "provisions": ["one", "two"],
            },
            CONSUMER: {
                "class": "arc_k8s" if arc_consumer else "docker_linux",
                "runners": {"instances": DECLARED_RUNNERS},
                "provisions": ["one"],
            },
        },
    }


def _clean(controller: ModuleType, data: dict[str, Any], host: str) -> frp.CommandResult:
    """Return the exact accepted check result for one host."""
    name = controller.recap_identity(data, host)
    producer = host == data["runner_image"]["source_host"]
    changed = controller.PRODUCER_CHECK_NOISE if producer else 0
    row = f"{name} : ok=9 changed={{}} unreachable=0 failed=0 skipped=1 rescued=0 ignored=0\n"
    plays = len(data["hosts"][host]["provisions"])
    return frp.CommandResult(0, row.format(changed) + row.format(0) * (plays - 1), "")


def _options(controller: ModuleType, state_dir: Path, now: int) -> object:
    """Return deterministic apply-mode policy for one pass at ``now``."""
    return controller.ReconcileOptions(
        mode="apply",
        force=False,
        source_digest=DIGEST,
        state_dir=state_dir,
        full_interval=FULL_INTERVAL,
        producer_interval=PRODUCER_INTERVAL,
        now=now,
    )


def _no_wait(_seconds: float) -> None:
    """Take retry pacing out of these cases without skipping it."""


def _seed(controller: ModuleType, state_dir: Path, *, passes: int | None) -> None:
    """Publish the state one earlier pass left behind, with or without a record."""
    document: dict[str, Any] = {"version": 1, "hosts": {}}
    if passes is not None:
        document["stranded"] = {CONSUMER: {"since": FIRST_PASS - 500, "passes": passes}}
    controller.save_state(state_dir / controller.STATE_FILE, document)


def _record(controller: ModuleType, state_dir: Path) -> dict[str, Any]:
    """Return the persisted stranded-at-zero map, however the pass ended."""
    document = controller.load_state(state_dir / controller.STATE_FILE)
    stored = document.get("stranded")
    return stored if isinstance(stored, dict) else {}


def _pass(
    controller: ModuleType,
    state_dir: Path,
    *,
    restore: str,
    arc_consumer: bool = False,
) -> tuple[int, dict[str, Any], list[tuple[str, str]]]:
    """Run one pass in which every host converges; only the consumer's restore varies.

    The consumer's check is CLEAN and it holds no receipt, so its apply is the
    periodic full verification and it SUCCEEDS: this is the happy path, not a
    recovery, and the restore it ends on is what these cases are about.
    """
    data = _data(arc_consumer=arc_consumer)
    calls: list[tuple[str, str]] = []

    def fake_run(argv: Sequence[str]) -> frp.CommandResult:
        verb, host = _identity(argv)
        calls.append((verb, host))
        if verb in {"check", "parked-check", "activation-check"}:
            return _clean(controller, data, host)
        if verb == "restore":
            return frp.CommandResult(0, restore if host == CONSUMER else FULL_TARGET, "")
        return frp.CommandResult(0, "", "")

    status = controller.reconcile(
        data, _options(controller, state_dir, FIRST_PASS), fake_run, _no_wait
    )
    return status, _record(controller, state_dir), calls


def _reconciled_at_zero_keeps_its_record(controller: ModuleType, failures: list[str]) -> None:
    """A success whose restore put nothing back leaves the host recorded at zero, loudly."""
    threshold = controller.STRANDED_ESCALATION_PASSES
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-claim-") as raw:
        state_dir = Path(raw)
        _seed(controller, state_dir, passes=threshold - 1)
        status, record, _calls = _pass(controller, state_dir, restore=ZERO_TARGET)
    entry = record.get(CONSUMER)
    if entry is None:
        failures.append("a reconciled host at ZERO admission had its record cleared as serving")
        return
    if entry["passes"] != threshold:
        failures.append(f"the record of a host left at zero by a success froze: {entry}")
    if status != controller.STRANDED_STATUS:
        failures.append(f"a host held at zero across passes did not escalate ({status})")


def _healthy_window_forges_no_record(controller: ModuleType, failures: list[str]) -> None:
    """A declared quiet-hours window is not this pass draining anything."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-claim-") as raw:
        state_dir = Path(raw)
        _seed(controller, state_dir, passes=None)
        status, record, _calls = _pass(controller, state_dir, restore=ZERO_TARGET)
    if record:
        failures.append(f"a declared quiet-hours target was recorded as stranding: {record}")
    if status:
        failures.append(f"a converged pass was failed by a host's own window policy ({status})")


def _real_capacity_still_clears(controller: ModuleType, failures: list[str]) -> None:
    """A restore that put real capacity back still clears the record it had."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-claim-") as raw:
        state_dir = Path(raw)
        _seed(controller, state_dir, passes=controller.STRANDED_ESCALATION_PASSES - 1)
        status, record, _calls = _pass(controller, state_dir, restore=FULL_TARGET)
    if record:
        failures.append(f"a host restored to real capacity stayed recorded at zero: {record}")
    if status:
        failures.append(f"a host climbing back off zero did not end its pass clean ({status})")


def _silent_restore_claims_nothing(controller: ModuleType, failures: list[str]) -> None:
    """An older capacity script reports no target, so nothing is claimed either way."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-claim-") as raw:
        state_dir = Path(raw)
        _seed(controller, state_dir, passes=controller.STRANDED_ESCALATION_PASSES - 1)
        status, record, _calls = _pass(controller, state_dir, restore=SILENT_RESTORE)
    if record:
        failures.append(f"a restore that reported no target was read as zero: {record}")
    if status:
        failures.append(f"a silent restore changed the verdict of a clean pass ({status})")


def _arc_apply_reads_its_own_restore(controller: ModuleType, failures: list[str]) -> None:
    """The ARC apply arm asks what its restore put back; it never used to ask at all."""
    threshold = controller.STRANDED_ESCALATION_PASSES
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-claim-") as raw:
        state_dir = Path(raw)
        _seed(controller, state_dir, passes=threshold - 1)
        run_arc = {"restore": ZERO_TARGET, "arc_consumer": True}
        status, record, calls = _pass(controller, state_dir, **run_arc)
    if ("activate", CONSUMER) not in calls:
        failures.append(f"the ARC apply never ran through its activation sequence: {calls}")
    if record.get(CONSUMER) is None:
        failures.append("an ARC host reconciled to ZERO admission was cleared as serving")
    elif status != controller.STRANDED_STATUS:
        failures.append(f"an ARC host held at zero across passes did not escalate ({status})")
    if any(verb == "quarantine" for verb, _host in calls):
        failures.append(f"a host already at zero admission was drained again: {calls}")


def _reader_stays_narrow(controller: ModuleType, failures: list[str]) -> None:
    """Unit-level: the transaction answers to the restore's own output and nothing else."""
    zero = frp.CommandResult(0, ZERO_TARGET, "")
    full = frp.CommandResult(0, FULL_TARGET, "")
    reading = controller.TransactionCapacity()
    if reading.read("parked-apply", zero) is not zero:
        failures.append("the transaction record did not hand its own result straight back")
    if reading.admission is not None:
        failures.append("an apply was read as a report about live admission")
    if reading.verb != "parked-apply":
        failures.append(f"a capacity mutation was not remembered: {reading.verb}")
    reading.read("restore", zero)
    if reading.admission != 0:
        failures.append("a restore that put nothing in service was not read as zero")
    reading.read("quarantine", full)
    if reading.admission != 0:
        failures.append("a verb that converges no admission overwrote the answer")
    reading.read("restore", full)
    if reading.admission != DECLARED_RUNNERS:
        failures.append("a restore that put real capacity back did not replace the answer")
    if controller.receipt_clock({"checked_at": FIRST_PASS}) != FIRST_PASS:
        failures.append("the receipt this pass publishes did not date its own record")
    if controller.receipt_clock({}) != 0:
        failures.append("a receipt with no stamp was read as a pass clock anyway")
    empty: dict[str, dict[str, int]] = {}
    if controller.hold_reconciled_at_zero(empty, CONSUMER, FIRST_PASS) or empty:
        failures.append(f"a host with no record was given one by a window target: {empty}")
    held = {CONSUMER: {"since": FIRST_PASS - 500, "passes": 1}}
    if not controller.hold_reconciled_at_zero(held, CONSUMER, FIRST_PASS):
        failures.append("an existing record was not aged by a restore that put nothing back")
    if held[CONSUMER]["passes"] != AGED_PASSES:
        failures.append(f"the aged record did not count this pass: {held}")


def run(controller: ModuleType) -> list[str]:
    """Return every claimed-capacity failure on the success path."""
    failures: list[str] = []
    _reconciled_at_zero_keeps_its_record(controller, failures)
    _healthy_window_forges_no_record(controller, failures)
    _real_capacity_still_clears(controller, failures)
    _silent_restore_claims_nothing(controller, failures)
    _arc_apply_reads_its_own_restore(controller, failures)
    _reader_stays_narrow(controller, failures)
    return failures
