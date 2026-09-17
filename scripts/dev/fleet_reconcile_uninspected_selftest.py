# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Regression selftests for a stop that freezes a stranded record (#888).

An administrative stop is ordinary, and the controller already accounts for it
honestly in two ways: ``report_uninspected`` names every host the pass never
reached, and ``stopped_verdict`` (#1360) lets the pass report the verdict it
earned instead of the signal status.

The record that verdict is read off was still standing still.  ``open_stranding``
never ages, and both places that do are reached by INSPECTING a host:
``invalidate_receipt`` for one that failed, and ``hold_at_zero`` for one the
pass deliberately held back.  A host the loop broke before reaching passed
through neither, so its consecutive-pass counter stayed exactly where the last
pass that got that far left it.

A stop lands where the work is, so the same stop lands at the same point run
after run: a systemd runtime limit against a slow producer apply, a maintenance
window closing on the same long step, the locked dependency downloads timing out
in issue #888's own evidence.  Every one of those passes ended with the hosts
behind that point still at ZERO capacity and their counters frozen below
``STRANDED_ESCALATION_PASSES``, so nothing escalated however long the fault
lasted: capacity at zero, no recovery, and an exit status that reads like any
one-off failure.

These tests pin what a pass cut short owes the hosts it never reached:

* a stop before a recorded host ages that host's record and escalates it on the
  pass that reaches the threshold;
* below the threshold the host is still aged and named, without a verdict;
* repeated stops at the same point escalate instead of freezing for good;
* a host with NO record is given none, because an uninspected host is not a
  drained one;
* a host this pass DID inspect is aged exactly once, never twice;
* check mode persists nothing, so it ages nothing and leaves the state file
  byte-for-byte as it found it.
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

NOW = 1000
FIRST_DRAINED_AT = 200
STALE_APPLIED_AT = 900
FULL_INTERVAL = 100
PRODUCER_INTERVAL = 50
DIGEST = "c" * 64
ORDINARY_FAILURE_STATUS = 1
REPEATED_STOPS = 4
BELOW_THRESHOLD_PASSES = 1

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
    """Return one producer and one capacity-managed consumer behind it."""
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


def _options(controller: ModuleType, state_dir: Path, mode: str = "apply") -> object:
    """Return deterministic policy for these cases.

    The controller arrives as a module, so its option dataclass is only handed
    straight back to it and never inspected in this file.
    """
    return controller.ReconcileOptions(
        mode=mode,
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


def _seed(
    controller: ModuleType,
    state_dir: Path,
    stranded: dict[str, dict[str, int]],
) -> None:
    """Seed a producer due for its pass and whatever record the case is about.

    The consumer carries no receipt on purpose: a host recorded at zero had its
    receipt dropped by the pass that drained it.
    """
    controller.save_state(
        state_dir / controller.STATE_FILE,
        {
            "version": 1,
            "hosts": {"producer": {"source_digest": DIGEST, "full_applied_at": STALE_APPLIED_AT}},
            "stranded": stranded,
        },
    )


def _stop_reading_producer(
    controller: ModuleType, data: dict[str, Any]
) -> Callable[[Sequence[str]], frp.CommandResult]:
    """Return a runner whose stop lands while the PRODUCER is being read.

    The loop then breaks at the top of the next iteration, so the consumer is
    never inspected: the host this suite is about.
    """

    def run(argv: Sequence[str]) -> frp.CommandResult:
        verb, host = _identity(argv)
        if verb == "check":
            if host == "producer":
                frp.STOP_STATE.process_signal = signal.SIGTERM
            noise = controller.PRODUCER_CHECK_NOISE if host == "producer" else 0
            return _check(controller, data, host, noise)
        return frp.CommandResult(0, "", "")

    return run


def _stopped_pass(
    controller: ModuleType,
    state_dir: Path,
    mode: str = "apply",
) -> tuple[int, dict[str, Any], str]:
    """Run one pass a stop cuts short over the producer, returning what it left."""
    data = _data()
    log = io.StringIO()
    with _stoppable(), contextlib.redirect_stderr(log), contextlib.redirect_stdout(io.StringIO()):
        status = controller.reconcile(
            data,
            _options(controller, state_dir, mode),
            _stop_reading_producer(controller, data),
            _no_wait,
        )
    document = controller.load_state(state_dir / controller.STATE_FILE)
    return status, document, log.getvalue()


def _passes(document: dict[str, Any], host: str) -> int | None:
    """Return a host's recorded consecutive passes at zero, if it has a record."""
    entry = document.get("stranded", {}).get(host)
    return entry["passes"] if isinstance(entry, dict) else None


def _case_stop_ages_the_host_it_never_reached(
    controller: ModuleType, state_dir: Path, failures: list[str]
) -> None:
    """One pass short of the threshold, a stop still has to count its pass."""
    prior = controller.STRANDED_ESCALATION_PASSES - 1
    _seed(controller, state_dir, {"consumer": {"since": FIRST_DRAINED_AT, "passes": prior}})
    status, document, log = _stopped_pass(controller, state_dir)
    if _passes(document, "consumer") != prior + 1:
        failures.append(
            "uninspected: a stop left a host at ZERO capacity without counting the "
            "pass, so its escalation counter freezes for as long as the stop lasts"
        )
    if status != controller.STRANDED_STATUS:
        failures.append(
            f"uninspected: a stop over a host reaching the escalation threshold exited {status}"
        )
    if "CRITICAL" not in log or "consumer" not in log:
        failures.append("uninspected: a fleet reaching the threshold behind a stop said nothing")


def _case_below_threshold_ages_without_a_verdict(
    controller: ModuleType, state_dir: Path, failures: list[str]
) -> None:
    """Ageing is not escalating: under the threshold the pass keeps its own verdict."""
    _seed(
        controller,
        state_dir,
        {"consumer": {"since": FIRST_DRAINED_AT, "passes": BELOW_THRESHOLD_PASSES}},
    )
    status, document, log = _stopped_pass(controller, state_dir)
    if _passes(document, "consumer") != BELOW_THRESHOLD_PASSES + 1:
        failures.append("uninspected: a below-threshold record was not aged by a stop")
    if status == controller.STRANDED_STATUS:
        failures.append("uninspected: a below-threshold record escalated behind a stop")
    if status != ORDINARY_FAILURE_STATUS:
        failures.append(f"uninspected: a stop below the threshold exited {status}")
    if "consumer" not in log:
        failures.append("uninspected: a host left at zero by a stop went unnamed")


def _case_repeated_stops_reach_the_threshold(
    controller: ModuleType, state_dir: Path, failures: list[str]
) -> None:
    """A stop landing at the same point every pass must not hide the fleet for good."""
    _seed(
        controller,
        state_dir,
        {"consumer": {"since": FIRST_DRAINED_AT, "passes": BELOW_THRESHOLD_PASSES}},
    )
    statuses = [_stopped_pass(controller, state_dir)[0] for _pass in range(REPEATED_STOPS)]
    if controller.STRANDED_STATUS not in statuses:
        failures.append(
            "uninspected: a stop reproducing at the same point kept a fleet at ZERO "
            f"capacity below its escalation threshold across {REPEATED_STOPS} passes"
        )


def _case_uninspected_is_not_drained(
    controller: ModuleType, state_dir: Path, failures: list[str]
) -> None:
    """A host with no record is given none: not reaching it proves nothing about it."""
    _seed(controller, state_dir, {})
    status, document, _log = _stopped_pass(controller, state_dir)
    if document.get("stranded"):
        failures.append(
            "uninspected: a stop forged a stranded-at-zero record for a host it never "
            "touched, which is the evidence the escalation and consumer release rest on"
        )
    if status == controller.STRANDED_STATUS:
        failures.append("uninspected: a stop over a healthy fleet escalated it")


def _case_an_inspected_host_ages_once(
    controller: ModuleType, state_dir: Path, failures: list[str]
) -> None:
    """The producer IS inspected before the stop, so it must not be aged twice."""
    prior = BELOW_THRESHOLD_PASSES
    _seed(controller, state_dir, {"producer": {"since": FIRST_DRAINED_AT, "passes": prior}})
    _status, document, _log = _stopped_pass(controller, state_dir)
    aged = _passes(document, "producer")
    if aged is not None and aged > prior + 1:
        failures.append(
            f"uninspected: a host this pass inspected was aged to {aged} from {prior}, "
            "counting one pass more than once"
        )


def _case_check_mode_persists_nothing(
    controller: ModuleType, state_dir: Path, failures: list[str]
) -> None:
    """A read-only pass counts no pass it will not write down."""
    prior = controller.STRANDED_ESCALATION_PASSES - 1
    _seed(controller, state_dir, {"consumer": {"since": FIRST_DRAINED_AT, "passes": prior}})
    state_file = state_dir / controller.STATE_FILE
    before = state_file.read_bytes()
    _status, document, _log = _stopped_pass(controller, state_dir, mode="check")
    if _passes(document, "consumer") != prior:
        failures.append("uninspected: a read-only pass aged a record it does not persist")
    if state_file.read_bytes() != before:
        failures.append("uninspected: a read-only pass cut short rewrote the state file")


Case = Callable[[ModuleType, Path, list[str]], None]

CASES: tuple[Case, ...] = (
    _case_stop_ages_the_host_it_never_reached,
    _case_below_threshold_ages_without_a_verdict,
    _case_repeated_stops_reach_the_threshold,
    _case_uninspected_is_not_drained,
    _case_an_inspected_host_ages_once,
    _case_check_mode_persists_nothing,
)


def run(controller: ModuleType) -> list[str]:
    """Return every failure from accounting for the hosts a stop never reached."""
    failures: list[str] = []
    for case in CASES:
        with tempfile.TemporaryDirectory(prefix="ra8-fleet-uninspected-") as raw:
            case(controller, Path(raw), failures)
    return failures
