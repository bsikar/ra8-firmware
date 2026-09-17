# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Regression selftests for which failures may hold back the fleet (issue #888).

Consumers are blocked after a producer failure because a half-finished image
build must not be handed to them.  The controller used to reach that verdict
from *any* producer failure, including a read-only check that mutated nothing:
an Ansible check that timed out left the producer serving its existing capacity
and still marked every consumer BLOCKED.  A consumer already sitting at zero
from an earlier pass could then never be repaired, which is the "TrueNAS runner
absent, needs re-registration" half of the issue: the fleet does not climb back
out on its own.

These tests pin where that line sits:

* a failed producer check fails the pass but leaves consumers reconciling;
* consumer capacity missing underneath that failed check is repaired in the
  same pass instead of waiting for the producer to become readable;
* a drained, drifting producer still blocks every consumer, exactly as before;
* an administrative stop during a check is still treated as stranded.
"""

from __future__ import annotations

import signal
import tempfile
from collections.abc import Sequence
from pathlib import Path
from types import ModuleType
from typing import Any

import fleet_reconcile_process as frp

CHECK_FAILURE_STATUS = 5
CONSUMER_DRIFT = 1
STALE_APPLIED_AT = 900
FRESH_APPLIED_AT = 975
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


def _data() -> dict[str, Any]:
    """Return one producer plus one consumer, both capacity-managed."""
    return {
        "runner_image": {"source_host": "producer"},
        "hosts": {
            "consumer": {
                "class": "docker_wsl",
                "runners": {"instances": 1},
                "provisions": ["one"],
            },
            "producer": {
                "class": "docker_linux",
                "runners": {"instances": 1},
                "provisions": ["one", "two"],
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


def _unreadable_producer_case(
    controller: ModuleType, state_dir: Path, *, consumer_drift: int
) -> tuple[int, list[tuple[str, str]], dict[str, Any]]:
    """Drive one pass whose producer check fails before anything is mutated."""
    data = _data()
    controller.save_state(
        state_dir / controller.STATE_FILE,
        {
            "version": 1,
            "hosts": {
                "producer": {"source_digest": DIGEST, "full_applied_at": STALE_APPLIED_AT},
                "consumer": {"source_digest": DIGEST, "full_applied_at": FRESH_APPLIED_AT},
            },
        },
    )
    calls: list[tuple[str, str]] = []
    consumer_checks = 0

    def fake_run(argv: Sequence[str]) -> frp.CommandResult:
        nonlocal consumer_checks
        verb, host = _identity(argv)
        calls.append((verb, host))
        if host == "producer" and verb == "check":
            return frp.CommandResult(CHECK_FAILURE_STATUS, "", "ssh: connect timed out\n")
        if verb in {"check", "parked-check"}:
            if verb == "check":
                consumer_checks += 1
            drifting = bool(consumer_drift) and verb == "check" and consumer_checks == 1
            return _check(controller, data, host, consumer_drift if drifting else 0)
        return frp.CommandResult(0, "", "")

    status = controller.reconcile(data, _options(controller, state_dir), fake_run, _no_wait)
    stored = controller.load_state(state_dir / controller.STATE_FILE)["hosts"]
    return status, calls, stored


def _failed_check_does_not_block(controller: ModuleType, failures: list[str]) -> None:
    """A read-only producer failure must fail the pass without holding the fleet."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-blocking-") as raw:
        status, calls, stored = _unreadable_producer_case(controller, Path(raw), consumer_drift=0)
    if status != 1:
        failures.append("an unreadable producer check was reported as success")
    if ("quarantine", "producer") in calls:
        failures.append("a read-only check failure drained capacity it never touched")
    if ("parked-apply", "producer") in calls:
        failures.append("an unreadable producer check was followed by a mutation")
    if ("check", "consumer") not in calls:
        failures.append("an unreadable producer check blocked every consumer")
    if "producer" in stored:
        failures.append("a failed producer check kept its receipt and skipped the next pass")


def _consumer_recovers_under_failed_check(controller: ModuleType, failures: list[str]) -> None:
    """Consumer capacity must be repairable while the producer stays unreadable."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-blocking-") as raw:
        status, calls, stored = _unreadable_producer_case(
            controller, Path(raw), consumer_drift=CONSUMER_DRIFT
        )
    if status != 1:
        failures.append("an unreadable producer check was reported as success")
    if ("parked-apply", "consumer") not in calls:
        failures.append("missing consumer capacity was left unrepaired by an unreadable producer")
    if ("restore", "consumer") not in calls:
        failures.append("a repaired consumer was never returned to service")
    if stored.get("consumer", {}).get("full_applied_at") != NOW:
        failures.append("a consumer repaired under a failed producer check published no receipt")


def _drained_producer_still_blocks(controller: ModuleType, failures: list[str]) -> None:
    """A drifting producer that drains must still hold every consumer back."""
    data = _data()
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-blocking-") as raw:
        state_dir = Path(raw)
        calls: list[tuple[str, str]] = []

        def fake_run(argv: Sequence[str]) -> frp.CommandResult:
            verb, host = _identity(argv)
            calls.append((verb, host))
            if verb in {"check", "parked-check"}:
                drift = controller.PRODUCER_CHECK_NOISE + CONSUMER_DRIFT
                return _check(controller, data, host, drift if host == "producer" else 0)
            return frp.CommandResult(1 if verb == "parked-apply" else 0, "", "")

        status = controller.reconcile(data, _options(controller, state_dir), fake_run, _no_wait)
    if status != 1:
        failures.append("a drained drifting producer was reported as success")
    if ("quarantine", "producer") not in calls:
        failures.append("a drifting producer that could not apply was left serving work")
    if any(host == "consumer" for _verb, host in calls):
        failures.append("a drained producer no longer blocked its consumers")


def _stop_during_check_is_stranded(controller: ModuleType, failures: list[str]) -> None:
    """An administrative stop mid-check must stay fail-closed for the fleet."""
    data = _data()

    def fake_run(argv: Sequence[str]) -> frp.CommandResult:
        verb, host = _identity(argv)
        if verb == "check":
            frp.STOP_STATE.process_signal = signal.SIGTERM
        return _check(controller, data, host, 0)

    try:
        ok, stranded, _receipt = controller.reconcile_host(
            data,
            "producer",
            None,
            _options(controller, Path("/unused")),
            fake_run,
            sleep=_no_wait,
        )
    finally:
        frp.STOP_STATE.process_signal = None
    if ok:
        failures.append("an interrupted check reported a converged host")
    if not stranded:
        failures.append("an administrative stop during a check stopped blocking the fleet")


def run(controller: ModuleType) -> list[str]:
    """Return every consumer-blocking failure."""
    failures: list[str] = []
    _failed_check_does_not_block(controller, failures)
    _consumer_recovers_under_failed_check(controller, failures)
    _drained_producer_still_blocks(controller, failures)
    _stop_during_check_is_stranded(controller, failures)
    return failures
