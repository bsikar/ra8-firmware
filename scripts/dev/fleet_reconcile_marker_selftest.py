# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Regression selftests for a zero-instance reopen dropping a park record (#888).

``clear_park`` is right that a landed ``capacity-restore`` is what removes a
host's durable maintenance marker, and both per-host transactions dropped the
parked record on that VERB alone.  The verb is not the evidence.  A restore
converges live admission to the host's CURRENT window target and prints it, and
``cmd_window`` forces that target to zero for exactly as long as the marker is
there ("maintenance: forcing target 0"), so a reopen on a PARKED host reporting
ZERO instances is the one shape that cannot tell a lifted park from a park
still holding the host down.  The controller read it as lifted.

Every other reopen already refuses that reading: both recovery arms, the
transaction that succeeds (``TransactionCapacity.served``), and the park
release that runs after the per-host loop, which holds the record and says so.
The two per-host paths were the last readers taking the verb on trust, and they
reach a parked host FIRST: the release only runs for a host whose declaration
verified this pass, and by then the record it works from is already gone.

Dropping it is how a park stops being recoverable.  ``parked_escalations`` has
nothing to escalate, ``report_durable_park`` says nothing,
``release_durable_parks`` never issues the one restore that lifts a park, and
``serving_hosts`` counts the host among those still holding capacity, so the
next pass may take real capacity to zero on a budget that includes a host
serving none.  The host stays pinned at zero by a marker its own window timer
cannot raise, and no later pass knows.

These cases pin the park record to what the reopen actually put back:

* the whole-pass repro: a parked host reconciled by a restore reporting ZERO
  keeps its record, the pass reports the park it could not lift, and the host
  escalates on the park clock instead of vanishing;
* a reopen that puts REAL capacity back still clears the record, so a host that
  genuinely came back does not stay permanently red;
* the same evidence holds on the failure path, where a refused drain after a
  restore reporting zero must not drop the record either;
* a host with no reopen verb at all is untouched, exactly as before;
* the reader itself answers only to the verb plus the admission it reported.
"""

from __future__ import annotations

import tempfile
from collections.abc import Sequence
from pathlib import Path
from types import ModuleType
from typing import Any

import fleet_reconcile_process as frp

NOW = 9000
EARLIER = 8000
FULL_INTERVAL = 1000
PRODUCER_INTERVAL = 500
DIGEST = "e" * 64
DECLARED_RUNNERS = 3
DRIFT_CHANGES = 4
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
            host: {
                "class": "docker_linux",
                "runners": {"instances": DECLARED_RUNNERS},
                "provisions": ["one"],
            }
            for host in (PRODUCER, CONSUMER)
        },
    }


def _recap(
    controller: ModuleType, data: dict[str, Any], host: str, changed: int
) -> frp.CommandResult:
    """Return one ansible recap for a host, at the drift this case wants."""
    name = controller.recap_identity(data, host)
    row = f"{name} : ok=9 changed={changed} unreachable=0 failed=0 skipped=1 rescued=0 ignored=0\n"
    return frp.CommandResult(0, row, "")


def _options(controller: ModuleType, state_dir: Path) -> object:
    """Return deterministic apply-mode policy for one pass at ``NOW``."""
    return controller.ReconcileOptions(
        mode="apply",
        force=False,
        source_digest=DIGEST,
        state_dir=state_dir,
        full_interval=FULL_INTERVAL,
        producer_interval=PRODUCER_INTERVAL,
        now=NOW,
    )


def _no_wait(_seconds: float) -> None:
    """Take retry pacing out of these cases without skipping it."""


def _seed(controller: ModuleType, state_dir: Path, *, passes: int) -> None:
    """Publish the parked record a refused drain left behind for the consumer."""
    controller.save_state(
        state_dir / controller.STATE_FILE,
        {
            "version": 1,
            "hosts": {},
            "parked": {CONSUMER: {"since": EARLIER - 500, "passes": passes}},
        },
    )


def _parked(controller: ModuleType, state_dir: Path) -> dict[str, Any]:
    """Return the persisted parked map, however the pass ended."""
    stored = controller.load_state(state_dir / controller.STATE_FILE).get("parked")
    return stored if isinstance(stored, dict) else {}


def _pass(
    controller: ModuleType,
    state_dir: Path,
    *,
    restore: str,
    refuse_drain: bool = False,
    fail_postcheck: bool = False,
) -> tuple[int, list[tuple[str, str]], dict[str, Any]]:
    """Run one pass in which the parked consumer drifts and is applied."""
    data = _data()
    calls: list[tuple[str, str]] = []
    applied: set[str] = set()

    def fake_run(argv: Sequence[str]) -> frp.CommandResult:
        verb, host = _identity(argv)
        calls.append((verb, host))
        noise = controller.PRODUCER_CHECK_NOISE if host == PRODUCER else 0
        if verb == "check":
            drifted = host == CONSUMER and host not in applied
            if host == CONSUMER and fail_postcheck and host in applied:
                return _recap(controller, data, host, noise + DRIFT_CHANGES)
            return _recap(controller, data, host, noise + DRIFT_CHANGES if drifted else noise)
        if verb == "parked-check":
            applied.add(host)
            return _recap(controller, data, host, noise)
        if verb == "restore":
            return frp.CommandResult(0, restore if host == CONSUMER else FULL_TARGET, "")
        if verb == "quarantine" and host == CONSUMER and refuse_drain:
            return frp.CommandResult(2, "", "capacity-quarantine refused\n")
        return frp.CommandResult(0, "", "")

    status = controller.reconcile(data, _options(controller, state_dir), fake_run, _no_wait)
    return status, calls, _parked(controller, state_dir)


def _zero_reopen_keeps_the_park(controller: ModuleType, failures: list[str]) -> None:
    """The repro: a reconciled park whose restore reported ZERO is still a park."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-marker-") as raw:
        state_dir = Path(raw)
        _seed(controller, state_dir, passes=controller.PARK_ESCALATION_PASSES - 1)
        status, calls, parked = _pass(controller, state_dir, restore=ZERO_TARGET)
    if ("restore", CONSUMER) not in calls:
        failures.append("the pass that was meant to reopen the parked consumer never did")
    entry = parked.get(CONSUMER)
    if entry is None:
        failures.append(
            "a reopen that put ZERO instances back dropped the durable park record, so "
            "nothing escalates the host and no later pass issues the restore that lifts it"
        )
        return
    if entry["passes"] != controller.PARK_ESCALATION_PASSES:
        failures.append(f"the parked record did not count this pass: {entry}")
    if status != controller.STRANDED_STATUS:
        failures.append(f"a park this pass could not lift did not escalate ({status})")


def _real_reopen_still_clears_the_park(controller: ModuleType, failures: list[str]) -> None:
    """A reopen that put real capacity back clears the record exactly as before."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-marker-") as raw:
        state_dir = Path(raw)
        _seed(controller, state_dir, passes=1)
        status, _calls, parked = _pass(controller, state_dir, restore=FULL_TARGET)
    if parked:
        failures.append(f"a host reopened with real capacity stayed parked: {parked}")
    if status:
        failures.append(f"a recovered fleet kept a red verdict ({status})")


def _refused_drain_after_zero_reopen(controller: ModuleType, failures: list[str]) -> None:
    """The failure path reads the same evidence: a zero reopen proves no lifted park."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-marker-") as raw:
        state_dir = Path(raw)
        _seed(controller, state_dir, passes=1)
        status, _calls, parked = _pass(
            controller, state_dir, restore=ZERO_TARGET, refuse_drain=True, fail_postcheck=True
        )
    if CONSUMER not in parked:
        failures.append(
            "a failed pass whose reopen reported ZERO instances dropped the parked record"
        )
    if status != controller.DRAIN_FAILED_STATUS:
        failures.append(f"a host nothing could drain did not earn the loudest verdict ({status})")


def _reader_stays_narrow(controller: ModuleType, failures: list[str]) -> None:
    """Unit-level: the record answers to the reopen verb AND what it put back."""
    parked = {CONSUMER: {"since": EARLIER, "passes": 1}}
    if controller.park_cleared_by_reopen(parked, CONSUMER, reopened=False, served=True):
        failures.append("a pass that issued no reopen verb reported clearing a park")
    if controller.park_cleared_by_reopen(parked, CONSUMER, reopened=True, served=False):
        failures.append("a reopen that put nothing in service reported clearing a park")
    if CONSUMER not in parked:
        failures.append("the parked record was dropped by a reopen with no capacity behind it")
    if not controller.park_cleared_by_reopen(parked, CONSUMER, reopened=True, served=True):
        failures.append("a reopen with real capacity behind it did not clear the park")
    if parked:
        failures.append(f"the cleared park record survived: {parked}")
    if controller.park_cleared_by_reopen(None, CONSUMER, reopened=True, served=True):
        failures.append("a pass with no parked record to clear reported clearing one")


def run(controller: ModuleType) -> list[str]:
    """Return every failure where a zero-instance reopen was read as a lifted park."""
    failures: list[str] = []
    _zero_reopen_keeps_the_park(controller, failures)
    _real_reopen_still_clears_the_park(controller, failures)
    _refused_drain_after_zero_reopen(controller, failures)
    _reader_stays_narrow(controller, failures)
    return failures
