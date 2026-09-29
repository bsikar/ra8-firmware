# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Regression selftests for a refused drain forging a zero-capacity record (#888).

Every failure path in the controller ends in a drain, and a drain the fleet
entry point REFUSES leaves the host unaccounted for: it may still be serving
work against a mutation that stopped halfway, which is why it earns the loudest
verdict the controller has.  The stranded-at-zero record makes the opposite
claim, that this controller took the host to zero and never reopened it, and it
was written on a refused drain as readily as on a drain that landed.  Three
later policies read that record back with no idea where it came from:

* a producer nobody could drain was treated as publishing nothing, so every
  consumer was released onto its "frozen" last-known-good image with a
  PROVISIONAL receipt, and a host left half-mutated is the one most likely to
  be mid-republish;
* the ``PRODUCER_BLOCK_PASSES`` delay, which exists to keep consumers off an
  image in flight, was spent by those passes, so the first pass that genuinely
  drained the producer released the consumers immediately;
* the pass drain budget treats a recorded host as having no capacity left to
  lose, so it neither counted nor protected a host that was still carrying
  work.

These tests pin the record to hosts this controller actually took to zero:

* refused-drain passes write no record at all and keep the unaccounted-for
  verdict, which is louder than being known to sit at zero;
* a producer nobody could drain freezes no image, so its consumers earn
  ordinary receipts;
* the producer block delay starts on the pass that really drained it;
* a record from a pass that really did drain the host stands unchanged when a
  later re-drain is refused, neither advanced nor cleared;
* a drain that LANDS still records the stranding and still spends the budget.
"""

from __future__ import annotations

import tempfile
from collections.abc import Sequence
from pathlib import Path
from types import ModuleType
from typing import Any

import fleet_reconcile_process as frp

APPLY_FAILURE_STATUS = 1
CHECK_FAILURE_STATUS = 2
DRAIN_REFUSED_STATUS = 7
ORDINARY_FAILURE_STATUS = 1
FIRST_PASS = 1000
PASS_INTERVAL = 100
FULL_INTERVAL = 100
PRODUCER_INTERVAL = 50
DIGEST = "a" * 64
LANDED_PASSES = 3
PRODUCER = "producer"
CONSUMER = "consumer"

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
                "runners": {"instances": 1},
                "provisions": ["one", "two"],
            },
            CONSUMER: {
                "class": "docker_linux",
                "runners": {"instances": 1},
                "provisions": ["one"],
            },
        },
    }


def _check(
    controller: ModuleType, data: dict[str, Any], host: str, changed: int
) -> frp.CommandResult:
    """Return one successful check whose first play reports ``changed``."""
    name = controller.recap_identity(data, host)
    row = f"{name} : ok=9 changed={{}} unreachable=0 failed=0 skipped=1 rescued=0 ignored=0\n"
    plays = len(data["hosts"][host]["provisions"])
    return frp.CommandResult(0, row.format(changed) + row.format(0) * (plays - 1), "")


def _clean(controller: ModuleType, data: dict[str, Any], host: str) -> frp.CommandResult:
    """Return the exact accepted check result for one host."""
    producer = host == data["runner_image"]["source_host"]
    return _check(controller, data, host, controller.PRODUCER_CHECK_NOISE if producer else 0)


def _drifting(controller: ModuleType, data: dict[str, Any], host: str) -> frp.CommandResult:
    """Return a check that reports one actionable change on the producer only."""
    producer = host == data["runner_image"]["source_host"]
    noise = controller.PRODUCER_CHECK_NOISE if producer else 0
    return _check(controller, data, host, noise + (1 if producer else 0))


def _options(controller: ModuleType, state_dir: Path, now: int) -> object:
    """Return deterministic apply-mode policy for one pass at ``now``.

    The controller arrives as a module, so its option dataclass is only handed
    straight back to it and never inspected in this file.
    """
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


def _document(controller: ModuleType, state_dir: Path) -> dict[str, Any]:
    """Return the persisted state document, however the pass ended."""
    return controller.load_state(state_dir / controller.STATE_FILE)


def _record(controller: ModuleType, state_dir: Path) -> dict[str, Any]:
    """Return the persisted stranded-at-zero map, however the pass ended."""
    stored = _document(controller, state_dir).get("stranded")
    return stored if isinstance(stored, dict) else {}


def _producer_apply_pass(
    controller: ModuleType, state_dir: Path, now: int, *, drain_refused: bool
) -> tuple[int, list[tuple[str, str]]]:
    """Run one pass whose drifting producer cannot apply; the consumer is healthy."""
    data = _data()
    calls: list[tuple[str, str]] = []

    def fake_run(argv: Sequence[str]) -> frp.CommandResult:
        verb, host = _identity(argv)
        calls.append((verb, host))
        if verb in {"check", "parked-check"}:
            return _drifting(controller, data, host)
        if verb == "parked-apply" and host == PRODUCER:
            return frp.CommandResult(APPLY_FAILURE_STATUS, "", "runner image build failed\n")
        if verb == "quarantine" and drain_refused:
            return frp.CommandResult(DRAIN_REFUSED_STATUS, "", "capacity API refused\n")
        return frp.CommandResult(0, "", "")

    status = controller.reconcile(data, _options(controller, state_dir, now), fake_run, _no_wait)
    return status, calls


def _producer_check_failure_pass(
    controller: ModuleType, state_dir: Path, now: int
) -> tuple[int, list[tuple[str, str]]]:
    """Run one pass whose producer check fails read-only, mutating nothing."""
    data = _data()
    calls: list[tuple[str, str]] = []

    def fake_run(argv: Sequence[str]) -> frp.CommandResult:
        verb, host = _identity(argv)
        calls.append((verb, host))
        if verb == "check" and host == PRODUCER:
            return frp.CommandResult(CHECK_FAILURE_STATUS, "", "ansible check timed out\n")
        if verb in {"check", "parked-check"}:
            return _clean(controller, data, host)
        return frp.CommandResult(0, "", "")

    status = controller.reconcile(data, _options(controller, state_dir, now), fake_run, _no_wait)
    return status, calls


def _refused_drain_records_no_zero_capacity(controller: ModuleType, failures: list[str]) -> None:
    """A host the controller could not drain must not be recorded as drained."""
    total = controller.STRANDED_ESCALATION_PASSES
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-unaccounted-") as raw:
        state_dir = Path(raw)
        statuses = [
            _producer_apply_pass(
                controller, state_dir, FIRST_PASS + index * PASS_INTERVAL, drain_refused=True
            )[0]
            for index in range(total)
        ]
        record = _record(controller, state_dir)
    if any(status != controller.DRAIN_FAILED_STATUS for status in statuses):
        failures.append(
            f"a host that could not be drained lost its unaccounted-for verdict ({statuses})"
        )
    if record:
        failures.append(f"a refused drain was recorded as zero capacity: {record}")


def _unaccounted_producer_freezes_no_image(controller: ModuleType, failures: list[str]) -> None:
    """A producer nobody drained may be republishing, so it freezes no image."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-unaccounted-") as raw:
        state_dir = Path(raw)
        _producer_apply_pass(controller, state_dir, FIRST_PASS, drain_refused=True)
        status, calls = _producer_check_failure_pass(
            controller, state_dir, FIRST_PASS + PASS_INTERVAL
        )
        receipt = _document(controller, state_dir)["hosts"].get(CONSUMER)
    if not isinstance(receipt, dict):
        failures.append(f"the consumer never reconciled past the failed check ({receipt})")
        return
    if controller.RELEASED_RECEIPT_KEY in receipt:
        failures.append(
            "a consumer earned a provisional receipt against the frozen image of a "
            f"producer nobody could drain: {receipt}"
        )
    if status != ORDINARY_FAILURE_STATUS:
        failures.append(f"a read-only producer check failure changed the verdict ({status})")
    if ("parked-apply", CONSUMER) not in calls:
        failures.append("the consumer was held back by a producer that lost no capacity")


def _block_delay_starts_when_capacity_is_lost(controller: ModuleType, failures: list[str]) -> None:
    """Refused passes must not spend the delay that keeps consumers off a live image."""
    total = controller.PRODUCER_BLOCK_PASSES
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-unaccounted-") as raw:
        state_dir = Path(raw)
        for index in range(total):
            _producer_apply_pass(
                controller, state_dir, FIRST_PASS + index * PASS_INTERVAL, drain_refused=True
            )
        status, calls = _producer_apply_pass(
            controller, state_dir, FIRST_PASS + total * PASS_INTERVAL, drain_refused=False
        )
        record = _record(controller, state_dir)
        receipt = _document(controller, state_dir)["hosts"].get(CONSUMER)
    if any(host == CONSUMER for _verb, host in calls):
        failures.append(
            "the first pass that really drained the producer released its consumers "
            f"immediately: {calls}"
        )
    if isinstance(receipt, dict) and controller.RELEASED_RECEIPT_KEY in receipt:
        failures.append(f"a consumer was released onto a just-frozen image: {receipt}")
    if record.get(PRODUCER, {}).get("passes") != 1:
        failures.append(
            f"the drained producer's record did not start on the pass that drained it: {record}"
        )
    # By this pass the producer has held its durable maintenance park across
    # ``PARK_ESCALATION_PASSES`` refused drains, so the park escalation is due
    # and ``STRANDED_STATUS`` is the honest verdict for a host nothing has
    # lifted off zero admission for that long.  What this case pins is the
    # BLOCK DELAY, which the assertions above carry.
    if status not in {
        ORDINARY_FAILURE_STATUS,
        controller.CASCADE_STATUS,
        controller.STRANDED_STATUS,
    }:
        failures.append(f"the pass that drained the producer earned {status}")


def _earlier_record_stands_unchanged(controller: ModuleType, failures: list[str]) -> None:
    """A record from a pass that really drained the host is neither advanced nor cleared."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-unaccounted-") as raw:
        state_dir = Path(raw)
        _producer_apply_pass(controller, state_dir, FIRST_PASS, drain_refused=False)
        landed = _record(controller, state_dir)
        status, _calls = _producer_apply_pass(
            controller, state_dir, FIRST_PASS + PASS_INTERVAL, drain_refused=True
        )
        after = _record(controller, state_dir)
    if landed.get(PRODUCER, {}).get("passes") != 1:
        failures.append(f"a drain that landed was not recorded: {landed}")
    if after.get(PRODUCER) != landed.get(PRODUCER):
        failures.append(f"a refused re-drain rewrote an existing zero-capacity record: {after}")
    if status != controller.DRAIN_FAILED_STATUS:
        failures.append(f"a refused re-drain lost the unaccounted-for verdict ({status})")


def _landed_drain_still_records(controller: ModuleType, failures: list[str]) -> None:
    """The record still counts every pass that genuinely took capacity to zero."""
    total = controller.STRANDED_ESCALATION_PASSES
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-unaccounted-") as raw:
        state_dir = Path(raw)
        statuses = [
            _producer_apply_pass(
                controller, state_dir, FIRST_PASS + index * PASS_INTERVAL, drain_refused=False
            )[0]
            for index in range(total)
        ]
        record = _record(controller, state_dir)
    entry = record.get(PRODUCER, {})
    if entry.get("passes") != total or entry.get("since") != FIRST_PASS:
        failures.append(f"drains that landed stopped being counted across passes: {record}")
    if statuses[-1] != controller.STRANDED_STATUS:
        failures.append(f"a fleet held at zero stopped escalating ({statuses})")


def _policy_stays_narrow(controller: ModuleType, failures: list[str]) -> None:
    """Unit-level: the record answers to capacity lost, nothing else."""
    empty: dict[str, dict[str, int]] = {}
    drained: list[str] = []
    controller.invalidate_receipt(
        {PRODUCER: {"checked_at": FIRST_PASS}},
        empty,
        drained,
        PRODUCER,
        FIRST_PASS,
        stranded=True,
        at_zero=False,
    )
    if empty or drained:
        failures.append(f"a refused drain invented a record: {empty} {drained}")
    held = {PRODUCER: {"since": FIRST_PASS, "passes": 2}}
    controller.invalidate_receipt(
        {}, held, drained, PRODUCER, FIRST_PASS + PASS_INTERVAL, stranded=True, at_zero=False
    )
    if held != {PRODUCER: {"since": FIRST_PASS, "passes": 2}} or drained:
        failures.append(f"a refused drain moved an existing record: {held} {drained}")
    controller.invalidate_receipt(
        {}, held, drained, PRODUCER, FIRST_PASS + PASS_INTERVAL, stranded=True
    )
    if held[PRODUCER]["passes"] != LANDED_PASSES or held[PRODUCER]["since"] != FIRST_PASS:
        failures.append(f"a drain that landed stopped counting: {held}")
    if drained:
        failures.append(f"a host already at zero spent the pass drain budget: {drained}")
    controller.invalidate_receipt({}, held, drained, CONSUMER, FIRST_PASS, stranded=True)
    if drained != [CONSUMER]:
        failures.append(f"a host taken from serving to zero did not spend the budget: {drained}")


def run(controller: ModuleType) -> list[str]:
    """Return every unaccounted-for capacity failure."""
    failures: list[str] = []
    _refused_drain_records_no_zero_capacity(controller, failures)
    _unaccounted_producer_freezes_no_image(controller, failures)
    _block_delay_starts_when_capacity_is_lost(controller, failures)
    _earlier_record_stands_unchanged(controller, failures)
    _landed_drain_still_records(controller, failures)
    _policy_stays_narrow(controller, failures)
    return failures
