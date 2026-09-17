# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Regression selftests for a fleet that stays at zero and stays quiet (#888).

The issue reports the Linux self-hosted runners stranded at zero capacity about
five separate times.  Every one of those passes exited 1, the same status an
ordinary one-off failure returns, and a failed pass deliberately drops the
host's receipt, so the state file carried no memory at all of a host being held
at zero.  A fleet that had idled at zero for days therefore looked exactly like
a fleet that had failed once a minute ago: the controller kept failing quietly
instead of getting louder, and nobody noticed until CI had no signal.

These tests pin the escalation:

* consecutive passes that leave a host drained are counted across passes, keep
  the time capacity was first lost, and escalate to ``STRANDED_STATUS`` once
  the fleet has failed to lift itself off zero;
* a pass that reconciles the host clears the record, so the next stranding
  starts from one instead of inheriting failures already recovered from;
* a read-only check failure drains nothing and must never be counted as lost
  capacity, nor may a consumer blocked behind a failed producer;
* a host that could not be drained at all keeps the louder unaccounted-for
  verdict, which outranks this one;
* state written by an older controller, or with a malformed record, still
  reconciles and simply starts counting from empty.
"""

from __future__ import annotations

import tempfile
from collections.abc import Sequence
from pathlib import Path
from types import ModuleType
from typing import Any

import fleet_reconcile_process as frp

APPLY_FAILURE_STATUS = 1
CHECK_FAILURE_STATUS = 5
DRAIN_REFUSED_STATUS = 7
ORDINARY_FAILURE_STATUS = 1
FIRST_PASS = 1000
PASS_INTERVAL = 100
FULL_INTERVAL = 100
PRODUCER_INTERVAL = 50
DIGEST = "a" * 64

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
        "runner_image": {"source_host": "producer"},
        "hosts": {
            "producer": {
                "class": "docker_linux",
                "runners": {"instances": 1},
                "provisions": ["one", "two"],
            },
            "consumer": {
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


def _stranded_record(controller: ModuleType, state_dir: Path) -> dict[str, Any]:
    """Return the persisted stranded-at-zero map, however the pass ended."""
    document = controller.load_state(state_dir / controller.STATE_FILE)
    stored = document.get("stranded")
    return stored if isinstance(stored, dict) else {}


def _drained_pass(
    controller: ModuleType, state_dir: Path, now: int, *, drain_refused: bool = False
) -> tuple[int, list[tuple[str, str]]]:
    """Run one pass whose drifting producer cannot apply and is drained."""
    data = _data()
    calls: list[tuple[str, str]] = []

    def fake_run(argv: Sequence[str]) -> frp.CommandResult:
        verb, host = _identity(argv)
        calls.append((verb, host))
        if verb in {"check", "parked-check"}:
            drift = 1 if host == "producer" else 0
            return _check(
                controller,
                data,
                host,
                (controller.PRODUCER_CHECK_NOISE if host == "producer" else 0) + drift,
            )
        if verb == "parked-apply":
            return frp.CommandResult(APPLY_FAILURE_STATUS, "", "runner image build failed\n")
        if verb == "quarantine" and drain_refused:
            return frp.CommandResult(DRAIN_REFUSED_STATUS, "", "capacity API refused\n")
        return frp.CommandResult(0, "", "")

    status = controller.reconcile(data, _options(controller, state_dir, now), fake_run, _no_wait)
    return status, calls


def _healthy_pass(controller: ModuleType, state_dir: Path, now: int) -> int:
    """Run one pass that converges every host and reopens its capacity."""
    data = _data()

    def fake_run(argv: Sequence[str]) -> frp.CommandResult:
        verb, host = _identity(argv)
        if verb in {"check", "parked-check"}:
            return _clean(controller, data, host)
        return frp.CommandResult(0, "", "")

    return controller.reconcile(data, _options(controller, state_dir, now), fake_run, _no_wait)


def _check_failure_pass(controller: ModuleType, state_dir: Path, now: int) -> int:
    """Run one pass whose producer check fails without mutating anything."""
    data = _data()

    def fake_run(argv: Sequence[str]) -> frp.CommandResult:
        verb, host = _identity(argv)
        if verb == "check" and host == "producer":
            return frp.CommandResult(CHECK_FAILURE_STATUS, "", "ansible check timed out\n")
        if verb in {"check", "parked-check"}:
            return _clean(controller, data, host)
        return frp.CommandResult(0, "", "")

    return controller.reconcile(data, _options(controller, state_dir, now), fake_run, _no_wait)


def _repeated_stranding_gets_louder(controller: ModuleType, failures: list[str]) -> None:
    """A fleet that cannot lift itself off zero must stop exiting like a blip."""
    total = controller.STRANDED_ESCALATION_PASSES
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-stranding-") as raw:
        state_dir = Path(raw)
        statuses = [
            _drained_pass(controller, state_dir, FIRST_PASS + index * PASS_INTERVAL)[0]
            for index in range(total)
        ]
        record = _stranded_record(controller, state_dir)
    if any(status != ORDINARY_FAILURE_STATUS for status in statuses[:-1]):
        failures.append("an early stranded pass escalated before the fleet had a chance to recover")
    if statuses[-1] != controller.STRANDED_STATUS:
        failures.append(
            f"a fleet stranded at zero across {total} passes exited {statuses[-1]}, "
            "the same status as a one-off failure"
        )
    if set(record) != {"producer"}:
        failures.append(
            f"the stranded-at-zero record named {sorted(record)}; a consumer blocked "
            "behind the producer was never drained and must not be counted"
        )
    entry = record.get("producer", {})
    if entry.get("passes") != total:
        failures.append("consecutive stranded passes were not counted across passes")
    if entry.get("since") != FIRST_PASS:
        failures.append("the stranded record lost the time capacity was first drained")


def _recovery_clears_the_record(controller: ModuleType, failures: list[str]) -> None:
    """A host proven to be serving again starts its next stranding from one."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-stranding-") as raw:
        state_dir = Path(raw)
        _drained_pass(controller, state_dir, FIRST_PASS)
        _drained_pass(controller, state_dir, FIRST_PASS + PASS_INTERVAL)
        healthy = _healthy_pass(controller, state_dir, FIRST_PASS + 2 * PASS_INTERVAL)
        cleared = _stranded_record(controller, state_dir)
        later = FIRST_PASS + 3 * PASS_INTERVAL
        status, _calls = _drained_pass(controller, state_dir, later)
        record = _stranded_record(controller, state_dir)
    if healthy:
        failures.append(f"a pass that reconciled every host exited {healthy}")
    if cleared:
        failures.append("a host back in service kept its stranded-at-zero record")
    if status != ORDINARY_FAILURE_STATUS:
        failures.append(
            f"a fresh stranding exited {status} on its first pass, inheriting failures "
            "the fleet had already recovered from"
        )
    entry = record.get("producer", {})
    if entry.get("passes") != 1 or entry.get("since") != later:
        failures.append("a stranding after a recovery did not restart its count")


def _untouched_capacity_is_not_stranding(controller: ModuleType, failures: list[str]) -> None:
    """A read-only check failure drains nothing, so it strands nothing."""
    passes = controller.STRANDED_ESCALATION_PASSES + 1
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-stranding-") as raw:
        state_dir = Path(raw)
        statuses = [
            _check_failure_pass(controller, state_dir, FIRST_PASS + index * PASS_INTERVAL)
            for index in range(passes)
        ]
        record = _stranded_record(controller, state_dir)
    if any(status != ORDINARY_FAILURE_STATUS for status in statuses):
        failures.append(
            "a repeated read-only check failure was escalated as a fleet stranded at "
            "zero, though no capacity was ever touched"
        )
    if record:
        failures.append(f"a pass that drained nothing recorded {sorted(record)} at zero")


def _undrained_hosts_keep_the_louder_verdict(controller: ModuleType, failures: list[str]) -> None:
    """An unaccounted-for host outranks a host known to be sitting at zero."""
    total = controller.STRANDED_ESCALATION_PASSES
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-stranding-") as raw:
        state_dir = Path(raw)
        statuses = [
            _drained_pass(
                controller, state_dir, FIRST_PASS + index * PASS_INTERVAL, drain_refused=True
            )[0]
            for index in range(total)
        ]
        record = _stranded_record(controller, state_dir)
    if any(status != controller.DRAIN_FAILED_STATUS for status in statuses):
        failures.append(
            "a host that could not be drained lost its unaccounted-for verdict to the "
            "stranded-at-zero one"
        )
    if record.get("producer", {}).get("passes") != total:
        failures.append("a refused drain was not counted as a pass that cost capacity")


def _older_state_starts_counting(controller: ModuleType, failures: list[str]) -> None:
    """State from before this record existed, or damaged, must not be trusted."""
    document: dict[str, Any] = {"version": 1, "hosts": {}, "stranded": "not-a-map"}
    if controller.load_stranding(document) != {} or document["stranded"] != {}:
        failures.append("a malformed stranded record was not replaced with an empty one")
    damaged: dict[str, Any] = {"version": 1, "hosts": {}, "stranded": {"producer": {"passes": 2}}}
    if controller.load_stranding(damaged) != {}:
        failures.append("a stranded entry with no first-drained time was trusted")
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-stranding-") as raw:
        state_dir = Path(raw)
        controller.save_state(
            state_dir / controller.STATE_FILE,
            {"version": 1, "hosts": {"producer": {"source_digest": DIGEST, "full_applied_at": 1}}},
        )
        status, _calls = _drained_pass(controller, state_dir, FIRST_PASS)
        record = _stranded_record(controller, state_dir)
    if status != ORDINARY_FAILURE_STATUS:
        failures.append(f"state written by an older controller failed the pass ({status})")
    if record.get("producer", {}).get("passes") != 1:
        failures.append("state written by an older controller never started counting")


def run(controller: ModuleType) -> list[str]:
    """Return every stranded-at-zero escalation failure."""
    failures: list[str] = []
    _repeated_stranding_gets_louder(controller, failures)
    _recovery_clears_the_record(controller, failures)
    _untouched_capacity_is_not_stranding(controller, failures)
    _undrained_hosts_keep_the_louder_verdict(controller, failures)
    _older_state_starts_counting(controller, failures)
    return failures
