# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Regression selftests for a drain that fails after a mutation (issue #888).

Draining a host whose mutation failed is the controller's fail-closed reflex,
and every failure path in ``fleet_reconcile`` ends in ``quarantine``.  That
call used to log one warning and return, so a drain the fleet entry point
refused was indistinguishable from a drain that landed: the pass reported an
ordinary failure, the operator read "quarantining host at zero capacity", and
the host kept accepting jobs against a mutation that stopped halfway.  It is
the inverse of the stranding in the issue, with the runner still serving.

These tests pin the louder behaviour:

* a producer whose drain fails ends the pass on its own exit status, not the
  ordinary failure status, and still holds its consumers back;
* a consumer whose drain fails does not stop the rest of the fleet being
  reconciled, because refusing to continue is how capacity gets stranded;
* a drain that lands keeps the ordinary failure status, so the new verdict
  stays narrow;
* a drain that fails while reopening last-known-good capacity is reported too,
  instead of being swallowed as "not recovered".
"""

from __future__ import annotations

import tempfile
from collections.abc import Sequence
from pathlib import Path
from types import ModuleType
from typing import Any

import fleet_reconcile_process as frp

DRAIN_FAILURE_STATUS = 7
APPLY_FAILURE_STATUS = 1
STALE_APPLIED_AT = 900
NOW = 1000
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


def _data(consumers: int = 1) -> dict[str, Any]:
    """Return one producer plus ``consumers`` capacity-managed consumers."""
    hosts: dict[str, Any] = {
        "producer": {
            "class": "docker_linux",
            "runners": {"instances": 1},
            "provisions": ["one", "two"],
        }
    }
    for index in range(consumers):
        hosts[f"consumer{index}"] = {
            "class": "docker_linux",
            "runners": {"instances": 1},
            "provisions": ["one"],
        }
    return {"runner_image": {"source_host": "producer"}, "hosts": hosts}


def _check(
    controller: ModuleType, data: dict[str, Any], host: str, changed: int
) -> frp.CommandResult:
    """Return one successful check whose first play reports ``changed``."""
    name = controller.recap_identity(data, host)
    row = f"{name} : ok=9 changed={{}} unreachable=0 failed=0 skipped=1 rescued=0 ignored=0\n"
    plays = len(data["hosts"][host]["provisions"])
    return frp.CommandResult(0, row.format(changed) + row.format(0) * (plays - 1), "")


def _options(controller: ModuleType, state_dir: Path) -> object:
    """Return deterministic apply-mode policy for these cases.

    The controller is loaded as a module here, so its option dataclass is only
    passed straight back to it and never inspected in this file.
    """
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


def _drifting_pass(
    controller: ModuleType,
    state_dir: Path,
    *,
    failing_host: str,
    drain_lands: bool,
    consumers: int = 1,
) -> tuple[int, list[tuple[str, str]], dict[str, Any]]:
    """Drive one pass where ``failing_host`` drifts and cannot apply."""
    data = _data(consumers)
    calls: list[tuple[str, str]] = []

    def fake_run(argv: Sequence[str]) -> frp.CommandResult:
        verb, host = _identity(argv)
        calls.append((verb, host))
        if verb in {"check", "parked-check"}:
            noise = controller.PRODUCER_CHECK_NOISE if host == "producer" else 0
            drift = noise + (1 if host == failing_host else 0)
            return _check(controller, data, host, drift)
        if host == failing_host and verb == "parked-apply":
            return frp.CommandResult(APPLY_FAILURE_STATUS, "", "image build failed\n")
        if host == failing_host and verb == "quarantine" and not drain_lands:
            return frp.CommandResult(DRAIN_FAILURE_STATUS, "", "capacity API refused\n")
        return frp.CommandResult(0, "", "")

    status = controller.reconcile(data, _options(controller, state_dir), fake_run, _no_wait)
    stored = controller.load_state(state_dir / controller.STATE_FILE)["hosts"]
    return status, calls, stored


def _producer_drain_failure_is_louder(controller: ModuleType, failures: list[str]) -> None:
    """A producer left serving after a failed mutation gets its own verdict."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-drain-") as raw:
        state_dir = Path(raw)
        status, calls, stored = _drifting_pass(
            controller, state_dir, failing_host="producer", drain_lands=False
        )
    if status != controller.DRAIN_FAILED_STATUS:
        failures.append(
            f"a producer that could not be drained exited {status}, "
            "indistinguishable from an ordinary failed pass"
        )
    if ("quarantine", "producer") not in calls:
        failures.append("a producer that failed to apply was never drained")
    if "producer" in stored:
        failures.append("an undrained producer kept its receipt")
    if any(host.startswith("consumer") for _verb, host in calls):
        failures.append("an undrained producer stopped blocking its consumers")


def _consumer_drain_failure_does_not_strand_the_rest(
    controller: ModuleType, failures: list[str]
) -> None:
    """One host left serving must not stop the fleet being reconciled."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-drain-") as raw:
        state_dir = Path(raw)
        status, calls, stored = _drifting_pass(
            controller,
            state_dir,
            failing_host="consumer0",
            drain_lands=False,
            consumers=2,
        )
    if status != controller.DRAIN_FAILED_STATUS:
        failures.append(f"an undrained consumer exited {status} instead of the drain status")
    if ("check", "consumer1") not in calls:
        failures.append("an undrained consumer stopped the rest of the fleet reconciling")
    if "consumer0" in stored:
        failures.append("an undrained consumer kept its receipt")
    if stored.get("consumer1", {}).get("checked_at") != NOW:
        failures.append("a healthy consumer lost its receipt to another host's failed drain")


def _landed_drain_keeps_ordinary_failure(controller: ModuleType, failures: list[str]) -> None:
    """A drain that lands must still read as an ordinary failed pass."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-drain-") as raw:
        state_dir = Path(raw)
        status, calls, _stored = _drifting_pass(
            controller, state_dir, failing_host="producer", drain_lands=True
        )
    if status != 1:
        failures.append(f"a drained producer exited {status} instead of the ordinary failure")
    if ("quarantine", "producer") not in calls:
        failures.append("a drifting producer that could not apply was left serving work")


def _reopen_drain_failure_is_reported(controller: ModuleType, failures: list[str]) -> None:
    """A failed drain while reopening last-known-good capacity must surface."""
    data = _data()
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-drain-") as raw:
        state_dir = Path(raw)
        controller.save_state(
            state_dir / controller.STATE_FILE,
            {
                "version": 1,
                "hosts": {
                    "producer": {"source_digest": DIGEST, "full_applied_at": STALE_APPLIED_AT}
                },
            },
        )
        calls: list[tuple[str, str]] = []

        def fake_run(argv: Sequence[str]) -> frp.CommandResult:
            verb, host = _identity(argv)
            calls.append((verb, host))
            reopened = ("restore", host) in calls[:-1]
            if verb in {"check", "parked-check"}:
                # Converged before the apply, so the failed periodic full
                # verification reopens last-known-good capacity.  The check
                # that verifies the reopen then finds the host drifting, which
                # sends it back to a drain the capacity API refuses.
                noise = controller.PRODUCER_CHECK_NOISE
                return _check(controller, data, host, noise + (1 if reopened else 0))
            if verb == "parked-apply":
                return frp.CommandResult(APPLY_FAILURE_STATUS, "", "registry timed out\n")
            if verb == "quarantine" and reopened:
                return frp.CommandResult(DRAIN_FAILURE_STATUS, "", "capacity API refused\n")
            return frp.CommandResult(0, "", "")

        status = controller.reconcile(data, _options(controller, state_dir), fake_run, _no_wait)
    if ("restore", "producer") not in calls:
        failures.append("a converged producer never had its last-known-good capacity reopened")
    if status != controller.DRAIN_FAILED_STATUS:
        failures.append(
            "a failed drain while reopening last-known-good capacity was swallowed "
            f"(exited {status})"
        )


def _quarantine_refuses_to_return_quietly(controller: ModuleType, failures: list[str]) -> None:
    """The drain helper itself must carry the host and status it failed with."""

    def failing_run(_argv: Sequence[str]) -> frp.CommandResult:
        return frp.CommandResult(DRAIN_FAILURE_STATUS, "", "capacity API refused\n")

    try:
        controller.quarantine("producer", failing_run)
    except controller.DrainFailedError as error:
        if error.host != "producer" or error.status != DRAIN_FAILURE_STATUS:
            failures.append("a failed drain did not name the host and status it failed with")
    else:
        failures.append("a failed drain returned as though the host had been drained")

    def landing_run(_argv: Sequence[str]) -> frp.CommandResult:
        return frp.CommandResult(0, "", "")

    try:
        controller.quarantine("producer", landing_run)
    except controller.DrainFailedError:
        failures.append("a drain that landed was reported as failed")


def run(controller: ModuleType) -> list[str]:
    """Return every failed-drain reporting failure."""
    failures: list[str] = []
    _producer_drain_failure_is_louder(controller, failures)
    _consumer_drain_failure_does_not_strand_the_rest(controller, failures)
    _landed_drain_keeps_ordinary_failure(controller, failures)
    _reopen_drain_failure_is_reported(controller, failures)
    _quarantine_refuses_to_return_quietly(controller, failures)
    return failures
