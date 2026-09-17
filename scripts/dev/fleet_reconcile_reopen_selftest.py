# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Regression selftests for a refused drain that skipped recovery (issue #888).

Draining a host whose mutation failed is the fail-closed reflex, and a drain
the capacity API refuses is now loud rather than a warning.  Raising it from
``quarantine`` moved the failure earlier than the recovery hook, though: a host
that was converged before its periodic apply failed never got its
last-known-good capacity reopened and verified, because the refused drain left
``apply_host`` before that hook could run.  The controller then reported the
host as unaccounted for while it was still serving exactly the declaration it
had converged on, and a producer in that state holds every consumer back.  That
is the stranding in the issue arriving through the newest failure path.

These tests pin the recovery attempt happening anyway:

* a converged producer whose apply fails and whose drain is refused reopens and
  verifies last-known-good capacity, ends the pass on the ordinary failure
  status, and stops blocking its consumers;
* a drifting producer whose drain is refused stays unaccounted for, because
  nothing proved what it is serving;
* a refused drain whose reopen does not verify stays unaccounted for too;
* the drain attempt still comes first, and a refused drain with no recovery
  hook still raises, so the louder verdict stays intact.
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
ORDINARY_FAILURE_STATUS = 1
STALE_APPLIED_AT = 900
FRESH_APPLIED_AT = 950
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
    """Return one producer and one capacity-managed consumer."""
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


def _options(controller: ModuleType, state_dir: Path) -> object:
    """Return deterministic apply-mode policy for these cases.

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
        now=NOW,
    )


def _no_wait(_seconds: float) -> None:
    """Take retry pacing out of these cases without skipping it."""


def _pass(
    controller: ModuleType,
    state_dir: Path,
    *,
    producer_drifts: bool = False,
    reopen_verifies: bool = True,
) -> tuple[int, list[tuple[str, str]], dict[str, Any]]:
    """Drive one pass whose producer cannot apply and cannot be drained."""
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

    def fake_run(argv: Sequence[str]) -> frp.CommandResult:
        verb, host = _identity(argv)
        calls.append((verb, host))
        reopened = ("restore", "producer") in calls[:-1]
        if verb in {"check", "parked-check"}:
            if host != "producer":
                return _check(controller, data, host, 0)
            drift = 1 if producer_drifts else 0
            drift += 1 if reopened and not reopen_verifies else 0
            return _check(controller, data, host, controller.PRODUCER_CHECK_NOISE + drift)
        if verb == "parked-apply":
            return frp.CommandResult(APPLY_FAILURE_STATUS, "", "registry timed out\n")
        if verb == "quarantine":
            return frp.CommandResult(DRAIN_FAILURE_STATUS, "", "capacity API refused\n")
        return frp.CommandResult(0, "", "")

    status = controller.reconcile(data, _options(controller, state_dir), fake_run, _no_wait)
    stored = controller.load_state(state_dir / controller.STATE_FILE)["hosts"]
    return status, calls, stored


def _refused_drain_still_reopens_last_known_good(
    controller: ModuleType, failures: list[str]
) -> None:
    """A converged producer left serving must be verified, not written off."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-reopen-") as raw:
        status, calls, stored = _pass(controller, Path(raw))
    if ("restore", "producer") not in calls:
        failures.append(
            "a refused drain skipped the last-known-good reopen, so a converged "
            "producer was held unaccounted for"
        )
    if status != ORDINARY_FAILURE_STATUS:
        failures.append(
            f"a producer serving verified last-known-good capacity exited {status} "
            "as though it were unaccounted for"
        )
    if ("check", "consumer") not in calls:
        failures.append("a producer with verified capacity still blocked its consumers")
    if "producer" in stored:
        failures.append("a producer whose apply failed kept its receipt")
    if stored.get("consumer", {}).get("checked_at") != NOW:
        failures.append("a healthy consumer lost its receipt to the producer's refused drain")


def _refused_drain_on_a_drifting_producer_stays_unaccounted(
    controller: ModuleType, failures: list[str]
) -> None:
    """Nothing proved what a drifting undrained host serves, so it stays loud."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-reopen-") as raw:
        status, calls, _stored = _pass(controller, Path(raw), producer_drifts=True)
    if ("restore", "producer") in calls:
        failures.append("a drifting producer had its drifted capacity reopened")
    if status != controller.DRAIN_FAILED_STATUS:
        failures.append(
            f"a drifting producer that could not be drained exited {status} instead "
            "of the undrained verdict"
        )
    if ("check", "consumer") in calls:
        failures.append("an undrained drifting producer stopped blocking its consumers")


def _refused_drain_with_an_unverified_reopen_stays_unaccounted(
    controller: ModuleType, failures: list[str]
) -> None:
    """Reopened capacity that does not verify is not capacity we can trust."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-reopen-") as raw:
        status, calls, _stored = _pass(controller, Path(raw), reopen_verifies=False)
    if ("restore", "producer") not in calls:
        failures.append("the unverified reopen case never reopened capacity")
    if status != controller.DRAIN_FAILED_STATUS:
        failures.append(
            f"a reopen that did not verify exited {status} instead of the undrained verdict"
        )


def _drain_is_attempted_before_recovery(controller: ModuleType, failures: list[str]) -> None:
    """Fail closed first: recovery runs after the drain, never instead of it."""
    data = _data()
    calls: list[tuple[str, str]] = []
    ran_after: list[bool] = []

    def failing_run(argv: Sequence[str]) -> frp.CommandResult:
        verb, host = _identity(argv)
        calls.append((verb, host))
        if verb == "quarantine":
            return frp.CommandResult(DRAIN_FAILURE_STATUS, "", "capacity API refused\n")
        return frp.CommandResult(APPLY_FAILURE_STATUS, "", "registry timed out\n")

    def accounted_for() -> bool:
        ran_after.append(("quarantine", "producer") in calls)
        return True

    try:
        applied, _changed = controller.apply_host(
            data,
            "producer",
            failing_run,
            expected_check_changes=controller.PRODUCER_CHECK_NOISE,
            on_mutation_exhausted=accounted_for,
        )
    except controller.DrainFailedError:
        failures.append("a host proven to be serving last-known-good capacity still raised")
    else:
        if applied:
            failures.append("a failed mutation reported success")
    if ran_after != [True]:
        failures.append("the recovery hook did not run exactly once after the drain attempt")


def _refused_drain_without_recovery_still_raises(
    controller: ModuleType, failures: list[str]
) -> None:
    """With nothing to prove the host is serving, the louder verdict stands."""
    data = _data()

    def failing_run(argv: Sequence[str]) -> frp.CommandResult:
        verb, _host = _identity(argv)
        if verb == "quarantine":
            return frp.CommandResult(DRAIN_FAILURE_STATUS, "", "capacity API refused\n")
        return frp.CommandResult(APPLY_FAILURE_STATUS, "", "registry timed out\n")

    try:
        controller.apply_host(
            data, "producer", failing_run, expected_check_changes=controller.PRODUCER_CHECK_NOISE
        )
    except controller.DrainFailedError as error:
        if error.host != "producer" or error.status != DRAIN_FAILURE_STATUS:
            failures.append("a refused drain did not name the host and status it failed with")
    else:
        failures.append("a refused drain with no recovery returned as though it had landed")


def run(controller: ModuleType) -> list[str]:
    """Return every failed-drain recovery failure."""
    failures: list[str] = []
    _refused_drain_still_reopens_last_known_good(controller, failures)
    _refused_drain_on_a_drifting_producer_stays_unaccounted(controller, failures)
    _refused_drain_with_an_unverified_reopen_stays_unaccounted(controller, failures)
    _drain_is_attempted_before_recovery(controller, failures)
    _refused_drain_without_recovery_still_raises(controller, failures)
    return failures
