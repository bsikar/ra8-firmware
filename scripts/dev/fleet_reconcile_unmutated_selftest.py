# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Regression selftests for a pass that MUTATES NOTHING clearing a zero record (#888).

A host whose read-only check comes back CURRENT with no full converge due is
reconciled by doing nothing at all: no ``parked-apply``, no
``capacity-restore``, no verb of any kind.  ``record_success`` published its
receipt and cleared the host's stranded-at-zero record as "reconciled and
serving again", which is a claim about capacity that pass holds no evidence
for.  The check reads the DECLARATION, and a live capacity change is temporary
by construction, so it verifies whatever admission the host actually holds; the
drain that wrote the record wrote the host's DURABLE maintenance marker first,
so ``cmd_window`` forces its target to zero and only a ``capacity-restore``
removes it.

That left the success path's own fix exactly one pass wide.  A reconciled host
whose restore reported ZERO instances keeps its record and publishes a receipt
stamped ``full_applied_at`` in the same breath, so the very NEXT pass finds
nothing due, mutates nothing, and drops the record it was just told to hold.
After that nothing is left to be loud about: ``stranded_escalations`` needs
``STRANDED_ESCALATION_PASSES`` consecutive passes and the counter is gone, the
drain budget counts the host among those still serving,
``frozen_image_release`` stops marking its consumers provisional, and the fleet
sits DECLARED for its runners while serving none of them at exit 0 for a whole
full-apply interval.

These cases pin the quietest pass there is to the capacity it left in service:

* a host recorded at zero whose pass issues no capacity verb keeps its record
  and ages it, so it still escalates at ``STRANDED_ESCALATION_PASSES``;
* the two-pass sequence that made it reachable holds end to end: the restore
  that reported zero, then the no-op pass after it;
* a host with NO record is given none, because doing nothing to a host is not
  draining it, and a record written here would forge the proof
  ``frozen_image_release``, ``consumers_released`` and the drain budget read
  straight back off it;
* a pass that really did reopen the host still clears the record exactly as
  before, so nothing turns a recovered fleet permanently red;
* the receipt a no-op pass republishes still refreshes ``checked_at`` and
  leaves ``full_applied_at`` alone, so the host converges when its interval is
  up rather than being pinned by this fix;
* the transaction reader and the new holder answer only to their own evidence.
"""

from __future__ import annotations

import tempfile
from collections.abc import Sequence
from pathlib import Path
from types import ModuleType
from typing import Any

import fleet_reconcile_process as frp

FIRST_PASS = 7000
NEXT_PASS = 7120
FULL_INTERVAL = 1000
PRODUCER_INTERVAL = 500
DIGEST = "e" * 64
DECLARED_RUNNERS = 3
AGED_PASSES = 2
PRODUCER = "producer"
CONSUMER = "consumer"
ZERO_TARGET = "2026-09-17T23:05:11Z restoring current window target 0\n"
FULL_TARGET = f"2026-09-17T11:05:11Z restoring current window target {DECLARED_RUNNERS}\n"

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


def _data() -> dict[str, Any]:
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
                "class": "docker_linux",
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


def _receipt(clock: int) -> dict[str, Any]:
    """Return the receipt one converged pass publishes for a host."""
    return {"checked_at": clock, "full_applied_at": clock, "source_digest": DIGEST}


def _seed(controller: ModuleType, state_dir: Path, *, passes: int | None, fresh: bool) -> None:
    """Publish the state an earlier pass left behind, with or without a record.

    ``fresh`` gives both hosts a receipt this pass will find nothing due
    against, which is what makes the pass mutate nothing at all.
    """
    document: dict[str, Any] = {"version": 1, "hosts": {}}
    if fresh:
        document["hosts"] = {PRODUCER: _receipt(FIRST_PASS), CONSUMER: _receipt(FIRST_PASS)}
    if passes is not None:
        document["stranded"] = {CONSUMER: {"since": FIRST_PASS - 500, "passes": passes}}
    controller.save_state(state_dir / controller.STATE_FILE, document)


def _state(controller: ModuleType, state_dir: Path) -> dict[str, Any]:
    """Return the persisted state document, however the pass ended."""
    return controller.load_state(state_dir / controller.STATE_FILE)


def _record(controller: ModuleType, state_dir: Path) -> dict[str, Any]:
    """Return the persisted stranded-at-zero map."""
    stored = _state(controller, state_dir).get("stranded")
    return stored if isinstance(stored, dict) else {}


def _pass(
    controller: ModuleType,
    state_dir: Path,
    now: int,
    *,
    restore: str = FULL_TARGET,
) -> tuple[int, dict[str, Any], list[tuple[str, str]]]:
    """Run one pass in which every host's check is CLEAN, and report what it issued."""
    data = _data()
    calls: list[tuple[str, str]] = []

    def fake_run(argv: Sequence[str]) -> frp.CommandResult:
        verb, host = _identity(argv)
        calls.append((verb, host))
        if verb in {"check", "parked-check", "activation-check"}:
            return _clean(controller, data, host)
        if verb == "restore":
            return frp.CommandResult(0, restore if host == CONSUMER else FULL_TARGET, "")
        return frp.CommandResult(0, "", "")

    status = controller.reconcile(data, _options(controller, state_dir, now), fake_run, _no_wait)
    return status, _record(controller, state_dir), calls


def _unmutated_pass_keeps_the_record(controller: ModuleType, failures: list[str]) -> None:
    """The whole-pass repro: nothing issued, so nothing claimed about capacity."""
    threshold = controller.STRANDED_ESCALATION_PASSES
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-unmutated-") as raw:
        state_dir = Path(raw)
        _seed(controller, state_dir, passes=threshold - 1, fresh=True)
        status, record, calls = _pass(controller, state_dir, FIRST_PASS + 10)
    mutations = [call for call in calls if call[0] != "check"]
    if mutations:
        failures.append(f"a converged pass with nothing due issued a capacity verb: {mutations}")
    entry = record.get(CONSUMER)
    if entry is None:
        failures.append(
            "a pass that issued no capacity verb cleared a zero-capacity record as serving"
        )
        return
    if entry["passes"] != threshold:
        failures.append(f"the record of a host no pass has lifted froze: {entry}")
    if status != controller.STRANDED_STATUS:
        failures.append(f"a host held at zero across passes did not escalate ({status})")


def _restore_at_zero_then_no_op(controller: ModuleType, failures: list[str]) -> None:
    """The sequence that made it reachable: a restore reporting zero, then a quiet pass."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-unmutated-") as raw:
        state_dir = Path(raw)
        _seed(controller, state_dir, passes=1, fresh=False)
        _first, held, first_calls = _pass(controller, state_dir, FIRST_PASS, restore=ZERO_TARGET)
        if ("restore", CONSUMER) not in first_calls:
            failures.append("the pass that was meant to restore the consumer never did")
        if CONSUMER not in held:
            failures.append("a restore that reported ZERO instances dropped the record")
        receipt = _state(controller, state_dir)["hosts"].get(CONSUMER)
        if not isinstance(receipt, dict) or receipt.get("full_applied_at") != FIRST_PASS:
            failures.append(f"the held pass did not publish its own receipt: {receipt}")
        status, record, calls = _pass(controller, state_dir, NEXT_PASS)
    if [call for call in calls if call[0] != "check"]:
        failures.append(f"the pass after the held one was not a no-op: {calls}")
    entry = record.get(CONSUMER)
    if entry is None:
        failures.append("the pass after a restore to ZERO cleared the record it was left")
        return
    if entry["passes"] != AGED_PASSES + 1:
        failures.append(f"the record did not count the quiet pass after it: {entry}")
    if status != controller.STRANDED_STATUS:
        failures.append(f"the fleet stopped escalating a host still at zero ({status})")


def _healthy_fleet_forges_no_record(controller: ModuleType, failures: list[str]) -> None:
    """Doing nothing to a host is not draining it, so no record is written."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-unmutated-") as raw:
        state_dir = Path(raw)
        _seed(controller, state_dir, passes=None, fresh=True)
        status, record, calls = _pass(controller, state_dir, FIRST_PASS + 10)
    if record:
        failures.append(f"a converged host with nothing due was recorded at zero: {record}")
    if status:
        failures.append(f"a fleet with nothing to do failed its pass ({status})")
    if [call for call in calls if call[0] != "check"]:
        failures.append(f"a healthy fleet was mutated by this pass: {calls}")


def _real_reopen_still_clears(controller: ModuleType, failures: list[str]) -> None:
    """A pass that really put capacity back still clears the record it had."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-unmutated-") as raw:
        state_dir = Path(raw)
        _seed(controller, state_dir, passes=controller.STRANDED_ESCALATION_PASSES, fresh=False)
        status, record, calls = _pass(controller, state_dir, FIRST_PASS)
    if ("restore", CONSUMER) not in calls:
        failures.append("the reopening pass never issued the restore it is about")
    if record:
        failures.append(f"a host reopened with real capacity stayed recorded at zero: {record}")
    if status:
        failures.append(f"a recovered fleet kept a red verdict ({status})")


def _no_op_receipt_keeps_its_interval(controller: ModuleType, failures: list[str]) -> None:
    """The republished receipt does not push the full apply the host still needs out."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-unmutated-") as raw:
        state_dir = Path(raw)
        _seed(controller, state_dir, passes=1, fresh=True)
        _pass(controller, state_dir, FIRST_PASS + 10)
        receipt = _state(controller, state_dir)["hosts"].get(CONSUMER)
    if not isinstance(receipt, dict):
        failures.append("a no-op pass dropped the receipt of a host recorded at zero")
        return
    if receipt.get("checked_at") != FIRST_PASS + 10:
        failures.append(f"the no-op pass did not stamp its own check: {receipt}")
    if receipt.get("full_applied_at") != FIRST_PASS:
        failures.append(f"a no-op pass claimed a full apply it never ran: {receipt}")


def _readers_stay_narrow(controller: ModuleType, failures: list[str]) -> None:
    """Unit-level: the transaction and the new holder answer to their own evidence only."""
    quiet = controller.TransactionCapacity()
    if quiet.served():
        failures.append("a transaction that issued no capacity verb claimed to be serving")
    quiet.read("parked-apply", frp.CommandResult(0, "", ""))
    if quiet.served():
        failures.append("an apply with no reopen verb was read as capacity in service")
    quiet.read("restore", frp.CommandResult(0, ZERO_TARGET, ""))
    if quiet.served():
        failures.append("a restore that put nothing in service was read as serving")
    quiet.read("restore", frp.CommandResult(0, FULL_TARGET, ""))
    if not quiet.served():
        failures.append("a restore that put real capacity back was not read as serving")
    silent = controller.TransactionCapacity()
    silent.read("restore", frp.CommandResult(0, "restored\n", ""))
    if not silent.served():
        failures.append("a restore that named no target stopped claiming its own reopen")
    empty: dict[str, dict[str, int]] = {}
    if controller.hold_unmutated_at_zero(empty, CONSUMER, FIRST_PASS) or empty:
        failures.append(f"a host with no record was given one by a quiet pass: {empty}")
    held = {CONSUMER: {"since": FIRST_PASS - 500, "passes": 1}}
    if not controller.hold_unmutated_at_zero(held, CONSUMER, FIRST_PASS):
        failures.append("an existing record was not aged by a pass that mutated nothing")
    if held[CONSUMER]["passes"] != AGED_PASSES:
        failures.append(f"the aged record did not count this pass: {held}")


def run(controller: ModuleType) -> list[str]:
    """Return every failure where a pass that mutated nothing claimed capacity."""
    failures: list[str] = []
    _unmutated_pass_keeps_the_record(controller, failures)
    _restore_at_zero_then_no_op(controller, failures)
    _healthy_fleet_forges_no_record(controller, failures)
    _real_reopen_still_clears(controller, failures)
    _no_op_receipt_keeps_its_interval(controller, failures)
    _readers_stay_narrow(controller, failures)
    return failures
