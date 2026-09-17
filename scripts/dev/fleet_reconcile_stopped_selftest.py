# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Regression selftests for a stop that hides a fleet held at zero (#888).

An administrative stop is ordinary.  A unit restart, an operator's Ctrl-C, a
systemd runtime limit cutting a slow pass short: the controller installs
handlers for TERM, HUP and INT, drains the host it was working on as its
fail-closed reflex, writes its per-host records, names every host it never
inspected, and exits ``128 + signal``.

That status is the honest account of a pass that was merely interrupted.  It
was also the ONLY thing such a pass could report.  ``main`` returned
``interrupted_status() or status``, so the signal outranked every verdict the
pass had earned: a host nobody could drain (``DRAIN_FAILED_STATUS``), a host
held at zero capacity across consecutive passes (``STRANDED_STATUS``), and a
pass that stopped mutating to keep the rest of the fleet serving
(``CASCADE_STATUS``) all came back as 143, which is exactly what a clean
``systemctl stop`` produces.  A stop that arrives at the same point on every
run, a runtime limit against a slow apply or the locked dependency downloads
timing out in issue #888's own evidence, therefore reported a fleet sitting at
zero as a routine shutdown for as long as it lasted.  Issue #888 went
unnoticed about five times on silence of exactly this shape, and this is the
last pass exit that had not been audited for it: the busy-lock exit was the
one before it (#1325).

These tests pin what a stopped pass has to report:

* a stop over hosts recorded at zero returns the stranded verdict, not the
  signal status, and says both out loud;
* a stop whose drain was REFUSED keeps the louder unaccounted-for verdict;
* a stop with no capacity at zero still reports the stop, because the stop is
  then the whole truth about the pass;
* the precedence is exactly the three zero-capacity verdicts, and only when a
  stop is actually pending;
* reading a verdict out writes nothing: the state file is untouched.
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
CLEAN_STATUS = 0
USAGE_STATUS = 2
LOCK_BUSY_STATUS = 75
STOP_STATUS = 128 + signal.SIGTERM
STALE_APPLIED_AT = 900
FRESH_APPLIED_AT = 950
NOW = 1000
FIRST_DRAINED_AT = 200
FULL_INTERVAL = 100
PRODUCER_INTERVAL = 50
DIGEST = "c" * 64
READING_STOP = "stop-while-reading"
REFUSED_DRAIN_STOP = "stop-then-refused-drain"

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


@contextlib.contextmanager
def _stopped() -> Iterator[None]:
    """Read a verdict out as a pass that has already been told to stop."""
    previous = frp.STOP_STATE.process_signal
    frp.STOP_STATE.process_signal = signal.SIGTERM
    try:
        yield
    finally:
        frp.STOP_STATE.process_signal = previous


def _seed(
    controller: ModuleType, state_dir: Path, stranded: dict[str, dict[str, int]] | None = None
) -> None:
    """Seed a converged producer that is due, a fresh consumer, and any record."""
    controller.save_state(
        state_dir / controller.STATE_FILE,
        {
            "version": 1,
            "hosts": {
                "producer": {"source_digest": DIGEST, "full_applied_at": STALE_APPLIED_AT},
                "consumer": {"source_digest": DIGEST, "full_applied_at": FRESH_APPLIED_AT},
            },
            "stranded": stranded or {},
        },
    )


def _stopped_pass(
    controller: ModuleType,
    state_dir: Path,
    *,
    stranded: dict[str, dict[str, int]] | None = None,
    shape: str = READING_STOP,
) -> tuple[int, int, str]:
    """Run one pass a stop cuts short, returning its verdict, exit status and log.

    ``shape`` picks where the stop lands: ``READING_STOP`` stops the pass while
    the producer is only being READ, so nothing was mutated and no capacity was
    lost, and ``REFUSED_DRAIN_STOP`` stops it during the mutation and then has
    the drain refused, which leaves the host unaccounted for.
    """
    data = _data()
    _seed(controller, state_dir, stranded)
    refused = shape == REFUSED_DRAIN_STOP
    stop_on = "parked-apply" if refused else "check"

    def run(argv: Sequence[str]) -> frp.CommandResult:
        verb, host = _identity(argv)
        if verb == stop_on:
            frp.STOP_STATE.process_signal = signal.SIGTERM
        if verb == "check":
            noise = controller.PRODUCER_CHECK_NOISE if host == "producer" else 0
            return _check(controller, data, host, noise + 1 if refused else noise)
        if verb == "parked-apply" and refused:
            return frp.CommandResult(APPLY_FAILURE_STATUS, "", "registry timed out\n")
        if verb == "quarantine" and refused:
            return frp.CommandResult(DRAIN_REFUSAL_STATUS, "", "capacity API refused\n")
        return frp.CommandResult(0, "", "")

    log = io.StringIO()
    with (
        _stoppable(),
        contextlib.redirect_stderr(log),
        contextlib.redirect_stdout(io.StringIO()),
    ):
        verdict = controller.reconcile(data, _options(controller, state_dir), run, _no_wait)
        status = controller.stopped_verdict(verdict)
    return verdict, status, log.getvalue()


def _case_stranded_outranks_the_stop(
    controller: ModuleType, state_dir: Path, failures: list[str]
) -> None:
    """A pass stopped over hosts held at zero must still report them."""
    held = {
        "consumer": {
            "since": FIRST_DRAINED_AT,
            "passes": controller.STRANDED_ESCALATION_PASSES + 1,
        }
    }
    verdict, status, log = _stopped_pass(controller, state_dir, stranded=held)
    if verdict != controller.STRANDED_STATUS:
        failures.append(f"stopped: a pass over a host held at zero earned {verdict}, expected 4")
    if status != controller.STRANDED_STATUS:
        failures.append(
            f"stopped: a pass over a host held at zero exited {status} instead of the "
            "stranded verdict, so a stop at the same point every run reports a fleet at "
            "zero as a routine shutdown"
        )
    if str(STOP_STATUS) not in log:
        failures.append("stopped: the verdict was reported without naming the stop behind it")
    document = controller.load_state(state_dir / controller.STATE_FILE)
    if "consumer" not in (document.get("stranded") or {}):
        failures.append("stopped: reporting the verdict dropped the record it was earned from")


def _case_undrained_outranks_the_stop(
    controller: ModuleType, state_dir: Path, failures: list[str]
) -> None:
    """A host nobody could drain is the loudest verdict here, stop or no stop."""
    verdict, status, _log = _stopped_pass(controller, state_dir, shape=REFUSED_DRAIN_STOP)
    if verdict != controller.DRAIN_FAILED_STATUS:
        failures.append(
            f"stopped: a stop during a mutation nobody could drain earned {verdict}, expected 3"
        )
    if status != controller.DRAIN_FAILED_STATUS:
        failures.append(
            f"stopped: a host that may still be serving work after a refused drain exited "
            f"{status} instead of the unaccounted-for verdict"
        )


def _case_stop_alone_still_reports_the_stop(
    controller: ModuleType, state_dir: Path, failures: list[str]
) -> None:
    """With no capacity at zero, the stop is the whole truth about the pass."""
    verdict, status, log = _stopped_pass(controller, state_dir)
    if verdict == controller.STRANDED_STATUS:
        failures.append("stopped: one stopped pass escalated a fleet that never left service")
    if status != STOP_STATUS:
        failures.append(
            f"stopped: a pass cut short with nothing at zero exited {status} instead of the "
            "stop status, which is the honest account of why it ended"
        )
    if "never inspected" not in log:
        failures.append("stopped: a pass cut short stopped naming the hosts it never reached")


def _case_only_zero_capacity_verdicts_outrank_a_stop(
    controller: ModuleType, _state_dir: Path, failures: list[str]
) -> None:
    """Unit level: exactly the three zero-capacity verdicts, and only under a stop."""
    louder = (
        controller.DRAIN_FAILED_STATUS,
        controller.STRANDED_STATUS,
        controller.CASCADE_STATUS,
    )
    failures.extend(
        f"stopped: verdict {verdict} stopped outranking the stop status"
        for verdict in louder
        if verdict not in controller.ZERO_CAPACITY_STATUSES
    )
    with _stopped(), contextlib.redirect_stderr(io.StringIO()):
        kept = {verdict: controller.stopped_verdict(verdict) for verdict in louder}
        quiet = {
            verdict: controller.stopped_verdict(verdict)
            for verdict in (CLEAN_STATUS, ORDINARY_FAILURE_STATUS, USAGE_STATUS, LOCK_BUSY_STATUS)
        }
    failures.extend(
        f"stopped: zero-capacity verdict {verdict} was reported as {reported}"
        for verdict, reported in kept.items()
        if reported != verdict
    )
    failures.extend(
        f"stopped: status {verdict} was reported as {reported} rather than the stop status, "
        "so an ordinary pass that was interrupted stopped reading as interrupted"
        for verdict, reported in quiet.items()
        if reported != STOP_STATUS
    )


def _case_no_stop_passes_every_verdict_through(
    controller: ModuleType, _state_dir: Path, failures: list[str]
) -> None:
    """Unit level: with no stop pending, nothing about the verdict changes."""
    with _stoppable(), contextlib.redirect_stderr(io.StringIO()):
        reported = {
            verdict: controller.stopped_verdict(verdict)
            for verdict in (
                CLEAN_STATUS,
                ORDINARY_FAILURE_STATUS,
                USAGE_STATUS,
                controller.DRAIN_FAILED_STATUS,
                controller.STRANDED_STATUS,
                controller.CASCADE_STATUS,
                LOCK_BUSY_STATUS,
            )
        }
    failures.extend(
        f"stopped: an uninterrupted pass reporting {verdict} came back as {actual}"
        for verdict, actual in reported.items()
        if actual != verdict
    )


def _case_reporting_a_verdict_writes_nothing(
    controller: ModuleType, state_dir: Path, failures: list[str]
) -> None:
    """Reading the verdict out is not a write: the state file is untouched."""
    held = {
        "consumer": {"since": FIRST_DRAINED_AT, "passes": controller.STRANDED_ESCALATION_PASSES}
    }
    _seed(controller, state_dir, held)
    state_file = state_dir / controller.STATE_FILE
    before = state_file.read_bytes()
    with _stopped(), contextlib.redirect_stderr(io.StringIO()):
        controller.stopped_verdict(controller.STRANDED_STATUS)
    if state_file.read_bytes() != before:
        failures.append("stopped: reporting a verdict rewrote the state file")


Case = Callable[[ModuleType, Path, list[str]], None]

CASES: tuple[Case, ...] = (
    _case_stranded_outranks_the_stop,
    _case_undrained_outranks_the_stop,
    _case_stop_alone_still_reports_the_stop,
    _case_only_zero_capacity_verdicts_outrank_a_stop,
    _case_no_stop_passes_every_verdict_through,
    _case_reporting_a_verdict_writes_nothing,
)


def run(controller: ModuleType) -> list[str]:
    """Run every stopped-pass reporting case against the controller under test."""
    failures: list[str] = []
    for case in CASES:
        with tempfile.TemporaryDirectory(prefix="ra8-fleet-stopped-") as raw:
            case(controller, Path(raw), failures)
    return failures
