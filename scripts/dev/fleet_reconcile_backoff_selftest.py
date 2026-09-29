# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Regression selftests for paced parked-apply retries (issue #888).

The producer's three mutation attempts used to be fired back to back with no
spacing at all, so one fault that lasts seconds rather than milliseconds (a
registry pull that times out, a mirror refusing connections) burned every
attempt inside the same fault window.  The host was then drained and, being the
image producer, it blocked every consumer: the fleet sat at zero capacity until
somebody noticed.

These tests pin the pacing itself:

* a transient failure is retried after a growing pause and never drains;
* exhausted attempts are paced between tries only, then fail closed;
* an administrative stop cuts a pause short instead of holding the service
  open for the whole backoff.
"""

from __future__ import annotations

import signal
from collections.abc import Sequence
from types import ModuleType
from typing import Any

import fleet_reconcile_process as frp

TRANSIENT_FAILURES = 2
APPLY_ATTEMPTS = 3
SINGLE_SLICE = 1


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
    """Return one producer plus the consumer these tests mutate."""
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


def _clean_check(controller: ModuleType, data: dict[str, Any], host: str) -> frp.CommandResult:
    """Return a converged check recap for one host."""
    identity = controller.recap_identity(data, host)
    rows = "".join(
        f"{identity} : ok=9 changed=0 unreachable=0 failed=0 skipped=1 rescued=0 ignored=0\n"
        for _ in data["hosts"][host]["provisions"]
    )
    return frp.CommandResult(0, rows, "")


def _apply_case(
    controller: ModuleType, *, failing_applies: int
) -> tuple[bool, list[tuple[str, str]], list[float]]:
    """Drive one parked apply whose first attempts fail, recording every pause."""
    data = _data()
    calls: list[tuple[str, str]] = []
    slices: list[float] = []
    applies = 0

    def fake_run(argv: Sequence[str]) -> frp.CommandResult:
        nonlocal applies
        verb, host = _identity(argv)
        calls.append((verb, host))
        if verb in {"check", "parked-check"}:
            return _clean_check(controller, data, host)
        if verb == "parked-apply":
            applies += 1
            return frp.CommandResult(1 if applies <= failing_applies else 0, "", "")
        return frp.CommandResult(0, "", "")

    applied, _changed = controller.apply_host(
        data,
        "consumer",
        fake_run,
        expected_check_changes=0,
        apply_attempts=APPLY_ATTEMPTS,
        sleep=slices.append,
    )
    return applied, calls, slices


def _transient_failure_is_paced(controller: ModuleType, failures: list[str]) -> None:
    """A fault that outlives one attempt must be retried later, not instantly."""
    applied, calls, slices = _apply_case(controller, failing_applies=TRANSIENT_FAILURES)
    expected = controller.retry_delay(1) + controller.retry_delay(TRANSIENT_FAILURES)
    if not applied:
        failures.append("a transient parked-apply failure was not recovered by paced retries")
    if ("quarantine", "consumer") in calls:
        failures.append("a recovered parked apply still drained its host")
    if sum(slices) != expected:
        failures.append(f"paced retries waited {sum(slices)}s instead of {expected}s")
    if any(value > controller.APPLY_RETRY_SLICE_SECONDS for value in slices):
        failures.append("a retry pause was taken in one uninterruptible sleep")


def _exhausted_retries_drain_once(controller: ModuleType, failures: list[str]) -> None:
    """Every attempt failing must pace between tries only, then fail closed."""
    applied, calls, slices = _apply_case(controller, failing_applies=APPLY_ATTEMPTS)
    expected = controller.retry_delay(1) + controller.retry_delay(TRANSIENT_FAILURES)
    if applied:
        failures.append("an exhausted parked apply reported success")
    if calls.count(("quarantine", "consumer")) != 1:
        failures.append("an exhausted parked apply did not drain its host exactly once")
    if sum(slices) != expected:
        failures.append("an exhausted parked apply paused after its final attempt")


def _stop_cuts_the_pause(controller: ModuleType, failures: list[str]) -> None:
    """An administrative stop must end the pause instead of holding the service."""
    data = _data()
    calls: list[tuple[str, str]] = []
    slices: list[float] = []

    def stopping_sleep(seconds: float) -> None:
        slices.append(seconds)
        frp.STOP_STATE.process_signal = signal.SIGTERM

    def fake_run(argv: Sequence[str]) -> frp.CommandResult:
        verb, host = _identity(argv)
        calls.append((verb, host))
        if verb in {"check", "parked-check"}:
            return _clean_check(controller, data, host)
        return frp.CommandResult(1 if verb == "parked-apply" else 0, "", "")

    try:
        applied, _changed = controller.apply_host(
            data,
            "consumer",
            fake_run,
            expected_check_changes=0,
            apply_attempts=APPLY_ATTEMPTS,
            sleep=stopping_sleep,
        )
    finally:
        frp.STOP_STATE.process_signal = None
    if applied:
        failures.append("an interrupted parked apply reported success")
    if len(slices) != SINGLE_SLICE:
        failures.append("an administrative stop did not cut the retry pause short")
    if [verb for verb, _host in calls].count("parked-apply") != SINGLE_SLICE:
        failures.append("an administrative stop was followed by another mutation attempt")
    if ("quarantine", "consumer") not in calls:
        failures.append("an interrupted parked apply did not drain its host")


def _delays_grow_and_stay_bounded(controller: ModuleType, failures: list[str]) -> None:
    """Pauses must grow per attempt and never exceed the declared ceiling."""
    delays = [controller.retry_delay(attempt) for attempt in range(1, 10)]
    if delays != sorted(delays):
        failures.append("retry pauses did not grow with each failed attempt")
    if max(delays) > controller.APPLY_RETRY_BACKOFF_CAP_SECONDS:
        failures.append("a retry pause exceeded its declared ceiling")
    if delays[0] != controller.APPLY_RETRY_BACKOFF_SECONDS:
        failures.append("the first retry pause drifted from its declared base")


def run(controller: ModuleType) -> list[str]:
    """Return every mutation-retry pacing failure."""
    failures: list[str] = []
    _transient_failure_is_paced(controller, failures)
    _exhausted_retries_drain_once(controller, failures)
    _stop_cuts_the_pause(controller, failures)
    _delays_grow_and_stay_bounded(controller, failures)
    return failures
