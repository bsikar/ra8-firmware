# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Regression selftests for an administrative stop mistaken for a drain (#888).

Every failure the controller records against a host is now load-bearing.  The
stranded-at-zero record decides when a fleet held at zero escalates, and it
decides when a producer's consumers are released to reconcile against its
frozen last-known-good image, because a DRAINED producer publishes nothing.

A stop that arrives while a host is being READ, before any mutation, was
reported exactly like a drain.  Nothing was mutated and the host kept serving,
yet the pass counted it as capacity lost.  Repeat that (a maintenance window
that keeps stopping the timer, an operator cutting passes short) and the
controller escalates a fleet that never left service, and worse, releases every
consumer onto the image of a producer that was never drained and may be halfway
through republishing it: the block that protects them rests on the record this
false entry forges.

These tests pin the account a stop earns:

* a stop during the read-only check fails the pass, drains nothing, records no
  stranding and keeps the host's escalation count at zero, while the pass
  keeps its fail-closed verdict;
* three stopped passes in a row never escalate and never release the
  consumers, because nothing ever proved the image frozen;
* a stop that arrives after the mutation started and drained the host still
  counts, while one whose drain was refused leaves the host unaccounted for
  rather than recorded at zero;
* every host the stop left uninspected is named, so a pass cut short can never
  read like a pass that looked at the whole fleet.
"""

from __future__ import annotations

import contextlib
import io
import signal
import tempfile
from collections.abc import Callable, Iterator, Sequence
from pathlib import Path
from types import ModuleType
from typing import Any

import fleet_reconcile_process as frp

APPLY_FAILURE_STATUS = 1
DRAIN_REFUSAL_STATUS = 7
ORDINARY_FAILURE_STATUS = 1
STALE_APPLIED_AT = 900
FRESH_APPLIED_AT = 950
NOW = 1000
FULL_INTERVAL = 100
PRODUCER_INTERVAL = 50
STOPPED_PASSES = 3
DIGEST = "c" * 64

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


@contextlib.contextmanager
def _stoppable() -> Iterator[None]:
    """Run one case that requests a stop, leaving no pending signal behind."""
    previous = frp.STOP_STATE.process_signal
    frp.STOP_STATE.process_signal = None
    try:
        yield
    finally:
        frp.STOP_STATE.process_signal = previous


def _receipts(controller: ModuleType, state_dir: Path) -> None:
    """Seed a converged producer that is due, and a consumer that is not."""
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


def _stopped_check_run(
    controller: ModuleType, data: dict[str, Any], calls: list[tuple[str, str]]
) -> Callable[[Sequence[str]], frp.CommandResult]:
    """Return a runner whose stop lands while the producer is being READ."""

    def run(argv: Sequence[str]) -> frp.CommandResult:
        verb, host = _identity(argv)
        calls.append((verb, host))
        if verb == "check":
            if host == "producer":
                frp.STOP_STATE.process_signal = signal.SIGTERM
            noise = controller.PRODUCER_CHECK_NOISE if host == "producer" else 0
            return _check(controller, data, host, noise)
        return frp.CommandResult(0, "", "")

    return run


def _stop_while_reading_records_nothing(controller: ModuleType, failures: list[str]) -> None:
    """A stop before any mutation costs no capacity, so it records none."""
    data = _data()
    calls: list[tuple[str, str]] = []
    log = io.StringIO()
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-interrupt-") as raw, _stoppable():
        state_dir = Path(raw)
        _receipts(controller, state_dir)
        with contextlib.redirect_stderr(log):
            status = controller.reconcile(
                data,
                _options(controller, state_dir),
                _stopped_check_run(controller, data, calls),
                _no_wait,
            )
        document = controller.load_state(state_dir / controller.STATE_FILE)
    if [call for call in calls if call[0] == "quarantine"]:
        failures.append("a stop during a read-only check drained the host it was reading")
    if document.get("stranded"):
        failures.append(
            "a stop before any mutation was recorded as capacity lost, which is the "
            "evidence the escalation and the consumer release both rest on"
        )
    if status == controller.STRANDED_STATUS:
        failures.append("one stopped pass escalated as though the fleet were held at zero")
    if "consumer" not in log.getvalue():
        failures.append("a stop cut the pass short without naming the hosts it never inspected")


def _repeated_stops_never_release_consumers(controller: ModuleType, failures: list[str]) -> None:
    """Nothing proved the image frozen, so the block must survive the stops."""
    data = _data()
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-interrupt-") as raw:
        state_dir = Path(raw)
        _receipts(controller, state_dir)
        for _pass in range(STOPPED_PASSES):
            calls: list[tuple[str, str]] = []
            with _stoppable(), contextlib.redirect_stderr(io.StringIO()):
                status = controller.reconcile(
                    data,
                    _options(controller, state_dir),
                    _stopped_check_run(controller, data, calls),
                    _no_wait,
                )
            if status == controller.STRANDED_STATUS:
                failures.append("stopped passes escalated a fleet that never left service")
        document = controller.load_state(state_dir / controller.STATE_FILE)
    stranding = document.get("stranded") or {}
    with contextlib.redirect_stderr(io.StringIO()):
        released = controller.consumers_released(stranding, "producer", undrained=False)
    if released:
        failures.append(
            "repeated stops released the consumers onto a producer image that was "
            "never frozen, because the producer was never drained"
        )


def _stopped_mutation_pass(
    controller: ModuleType, data: dict[str, Any], *, drain_refused: bool
) -> tuple[int, dict[str, Any]]:
    """Run one pass whose apply is cut short by a stop, then drained."""

    def run(argv: Sequence[str]) -> frp.CommandResult:
        verb, host = _identity(argv)
        if verb == "check":
            noise = controller.PRODUCER_CHECK_NOISE if host == "producer" else 0
            return _check(controller, data, host, noise)
        if verb == "parked-apply":
            frp.STOP_STATE.process_signal = signal.SIGTERM
            return frp.CommandResult(APPLY_FAILURE_STATUS, "", "registry timed out\n")
        if verb == "quarantine" and drain_refused:
            return frp.CommandResult(DRAIN_REFUSAL_STATUS, "", "capacity API refused\n")
        return frp.CommandResult(0, "", "")

    with tempfile.TemporaryDirectory(prefix="ra8-fleet-interrupt-") as raw, _stoppable():
        state_dir = Path(raw)
        _receipts(controller, state_dir)
        with contextlib.redirect_stderr(io.StringIO()):
            status = controller.reconcile(data, _options(controller, state_dir), run, _no_wait)
        document = controller.load_state(state_dir / controller.STATE_FILE)
    return status, document.get("stranded") or {}


def _stop_after_the_mutation_still_counts(controller: ModuleType, failures: list[str]) -> None:
    """A stop that reaches a mutation still earns the fail-closed verdict.

    A stop that reached a mutation and then DRAINED the host really did take
    capacity to zero, so it is recorded.  When the drain is refused instead,
    the host is unaccounted for rather than at zero: it earns the louder
    verdict and no zero-capacity record, because nothing proved it stopped
    serving.
    """
    data = _data()
    drained_status, drained_record = _stopped_mutation_pass(controller, data, drain_refused=False)
    refused_status, refused_record = _stopped_mutation_pass(controller, data, drain_refused=True)
    if "producer" not in drained_record:
        failures.append("a stop that reached a mutation stopped counting as capacity lost")
    if drained_status != ORDINARY_FAILURE_STATUS:
        failures.append(f"a stop during a mutation that drained the host exited {drained_status}")
    if refused_status != controller.DRAIN_FAILED_STATUS:
        failures.append(
            f"a stop during a mutation nobody could drain exited {refused_status} instead of "
            "the undrained verdict"
        )
    if refused_record:
        failures.append(
            f"a stop during a mutation nobody could drain was recorded at zero: {refused_record}"
        )


def _only_a_mutation_can_lose_capacity(controller: ModuleType, failures: list[str]) -> None:
    """Unit level: the record needs an issued mutation, not just a failure."""
    failures.extend(
        f"{verb} stopped counting as a verb that moves capacity"
        for verb in ("parked-apply", "quarantine", "restore", "activate")
        if not controller.capacity_mutation(verb)
    )
    failures.extend(
        f"the read-only verb {verb} was treated as a capacity mutation"
        for verb in ("check", "parked-check", "activation-check")
        if controller.capacity_mutation(verb)
    )
    with contextlib.redirect_stderr(io.StringIO()):
        unmutated = controller.capacity_lost("producer", stranded=True, mutated=False)
        mutated = controller.capacity_lost("producer", stranded=True, mutated=True)
        clean = controller.capacity_lost("producer", stranded=False, mutated=True)
    if unmutated:
        failures.append("a failure that issued no mutation still claimed it lost capacity")
    if not mutated:
        failures.append("a drained host stopped being recorded as stranded at zero")
    if clean:
        failures.append("a failure that kept its capacity was recorded as stranded")


def run(controller: ModuleType) -> list[str]:
    """Return every failure from accounting for an administrative stop."""
    failures: list[str] = []
    _stop_while_reading_records_nothing(controller, failures)
    _repeated_stops_never_release_consumers(controller, failures)
    _stop_after_the_mutation_still_counts(controller, failures)
    _only_a_mutation_can_lose_capacity(controller, failures)
    return failures
