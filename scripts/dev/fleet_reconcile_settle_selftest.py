# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Regression selftests for a drain refused twice in one pass (issue #888).

A host whose mutation is exhausted is drained, and a host that was converged
before that mutation failed then has its last-known-good capacity reopened and
verified.  A reopen that does not verify drains again, so one pass can be
refused two separate drains.  That second refusal raised straight out of
``settle_exhausted_mutation``, past the accounting the first refusal was
waiting on, so the first one was dropped: the operator saw a single status code
for a host the controller had failed to take out of service twice, with nothing
saying the host had been reopened in between.  A fleet held at zero went
unnoticed five times in the issue precisely because it never read louder than a
one-off failure.

These tests pin the full account:

* an unverified reopen whose own drain is refused ends the pass on the
  undrained verdict, keeps the producer blocking its consumers, and does not
  cost a blocked consumer its receipt;
* both refusals survive, the second raised with the first as its cause, so
  neither status code is lost;
* a refused recovery drain after a drain that landed raises on its own, with no
  invented cause;
* a refused first drain whose recovery verifies still returns normally, so the
  quieter verdict for a host proven to be serving is unchanged.
"""

from __future__ import annotations

import tempfile
from collections.abc import Callable, Sequence
from pathlib import Path
from types import ModuleType
from typing import Any

import fleet_reconcile_process as frp

FIRST_DRAIN_REFUSAL = 7
SECOND_DRAIN_REFUSAL = 9
APPLY_FAILURE_STATUS = 1
RESTORE_FAILURE_STATUS = 4
STALE_APPLIED_AT = 900
FRESH_APPLIED_AT = 950
NOW = 1000
FULL_INTERVAL = 100
PRODUCER_INTERVAL = 50
DIGEST = "b" * 64

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


def _refusing_run(
    controller: ModuleType, data: dict[str, Any], calls: list[tuple[str, str]], *, first: int
) -> Callable[[Sequence[str]], frp.CommandResult]:
    """Return a runner whose producer cannot apply, reopen, or be drained.

    ``first`` is the status the first drain is refused with; the drain that
    follows the unverified reopen is always refused.
    """

    def drained(argv: Sequence[str]) -> frp.CommandResult:
        """Refuse the reopen's drain always, and the first one on demand."""
        del argv
        attempted = len([call for call in calls if call == ("quarantine", "producer")])
        status = first if attempted == 1 else SECOND_DRAIN_REFUSAL
        return frp.CommandResult(status, "", "capacity API refused\n")

    refused = {
        "parked-apply": lambda _argv: frp.CommandResult(
            APPLY_FAILURE_STATUS, "", "registry timed out\n"
        ),
        "restore": lambda _argv: frp.CommandResult(
            RESTORE_FAILURE_STATUS, "", "capacity API refused\n"
        ),
        "quarantine": drained,
    }

    def run(argv: Sequence[str]) -> frp.CommandResult:
        verb, host = _identity(argv)
        calls.append((verb, host))
        if verb in {"check", "parked-check"}:
            noise = controller.PRODUCER_CHECK_NOISE if host == "producer" else 0
            return _check(controller, data, host, noise)
        return refused.get(verb, lambda _argv: frp.CommandResult(0, "", ""))(argv)

    return run


def _twice_refused_pass_is_unaccounted_for(controller: ModuleType, failures: list[str]) -> None:
    """Two refused drains in one pass leave the host out of service's reach."""
    data = _data()
    calls: list[tuple[str, str]] = []
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-settle-") as raw:
        state_dir = Path(raw)
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
        status = controller.reconcile(
            data,
            _options(controller, state_dir),
            _refusing_run(controller, data, calls, first=FIRST_DRAIN_REFUSAL),
            _no_wait,
        )
        stored = controller.load_state(state_dir / controller.STATE_FILE)["hosts"]
    drains = len([call for call in calls if call == ("quarantine", "producer")])
    expected_drains = 2
    if drains != expected_drains:
        failures.append(
            f"a refused drain followed by an unverified reopen attempted {drains} "
            "drains instead of one for the exhausted mutation and one for the reopen"
        )
    if status != controller.DRAIN_FAILED_STATUS:
        failures.append(
            f"a host refused two drains in one pass exited {status} instead of the "
            "undrained verdict"
        )
    if ("check", "consumer") in calls:
        failures.append("a producer nobody could drain stopped blocking its consumers")
    if "producer" in stored:
        failures.append("a producer that could not be drained kept its receipt")
    if stored.get("consumer", {}).get("full_applied_at") != FRESH_APPLIED_AT:
        failures.append("a blocked consumer lost its receipt to the producer's refused drains")


def _both_refusals_survive(controller: ModuleType, failures: list[str]) -> None:
    """Neither status code may be dropped: the pass refused both drains."""
    data = _data()
    calls: list[tuple[str, str]] = []
    run = _refusing_run(controller, data, calls, first=FIRST_DRAIN_REFUSAL)

    def reopen() -> bool:
        controller.recover_last_known_good(
            data,
            "producer",
            run,
            controller.PRODUCER_CHECK_NOISE,
            held_check_changes=controller.PRODUCER_CHECK_NOISE,
        )
        return False

    try:
        controller.settle_exhausted_mutation("producer", run, reopen)
    except controller.DrainFailedError as error:
        if error.status != SECOND_DRAIN_REFUSAL:
            failures.append(
                f"the raised refusal carried rc={error.status} instead of the drain "
                "that was refused last"
            )
        cause = error.__cause__
        if not isinstance(cause, controller.DrainFailedError):
            failures.append("the first refused drain was dropped instead of raised as the cause")
        elif cause.status != FIRST_DRAIN_REFUSAL:
            failures.append(
                f"the cause carried rc={cause.status} instead of the first refused drain"
            )
    else:
        failures.append("a host refused two drains settled as though one of them had landed")


def _recovery_refusal_after_a_landed_drain_raises_alone(
    controller: ModuleType, failures: list[str]
) -> None:
    """A landed first drain is not a cause, so none is invented for it."""
    data = _data()
    calls: list[tuple[str, str]] = []
    run = _refusing_run(controller, data, calls, first=0)

    def reopen() -> bool:
        controller.recover_last_known_good(
            data,
            "producer",
            run,
            controller.PRODUCER_CHECK_NOISE,
            held_check_changes=controller.PRODUCER_CHECK_NOISE,
        )
        return False

    try:
        controller.settle_exhausted_mutation("producer", run, reopen)
    except controller.DrainFailedError as error:
        if error.host != "producer" or error.status != SECOND_DRAIN_REFUSAL:
            failures.append("the refused recovery drain did not name its host and status")
        if error.__cause__ is not None:
            failures.append("a drain that landed was reported as the cause of the refused one")
    else:
        failures.append("a refused recovery drain returned as though it had landed")


def _verified_recovery_still_returns(controller: ModuleType, failures: list[str]) -> None:
    """A host proven to be serving keeps the quieter verdict it earned."""
    calls: list[tuple[str, str]] = []

    def run(argv: Sequence[str]) -> frp.CommandResult:
        verb, _host = _identity(argv)
        calls.append((verb, _host))
        if verb == "quarantine":
            return frp.CommandResult(FIRST_DRAIN_REFUSAL, "", "capacity API refused\n")
        return frp.CommandResult(0, "", "")

    try:
        controller.settle_exhausted_mutation("producer", run, lambda: True)
    except controller.DrainFailedError:
        failures.append("a host serving verified last-known-good capacity still raised")
    if [call for call in calls if call[0] == "quarantine"] != [("quarantine", "producer")]:
        failures.append("the exhausted mutation did not attempt exactly one drain")


def run(controller: ModuleType) -> list[str]:
    """Return every failure from settling a mutation nobody could drain."""
    failures: list[str] = []
    _twice_refused_pass_is_unaccounted_for(controller, failures)
    _both_refusals_survive(controller, failures)
    _recovery_refusal_after_a_landed_drain_raises_alone(controller, failures)
    _verified_recovery_still_returns(controller, failures)
    return failures
