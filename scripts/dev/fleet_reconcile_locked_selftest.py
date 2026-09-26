# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Regression selftests for a lock-busy pass that reports no zero capacity (#888).

Another controller owning the dev-host fleet mutation lock is ordinary and
expected: an operator's own ``just infra::apply``, a capacity change by hand,
or the previous reconcile pass still finishing.  The pass returned
``fleet_mutation_lock.LOCK_BUSY_STATUS`` before it read anything at all,
though, and that status is EX_TEMPFAIL: retry later, somebody else is working.

Over a fleet this controller has already taken to ZERO capacity it says the
wrong thing entirely.  The stranded-at-zero and durable-park records exist
because a fleet held at zero has to get LOUDER rather than quieter, and a lock
holder that is stuck, crashed, or merely long-running silenced both of them for
as long as it lasts: every pass exited 75, said nothing whatever about the
hosts sitting at zero, and nothing ever escalated.  That is the silence issue
#888 went unnoticed in five times, arriving before the pass even starts.

Reporting those records persists nothing and mutates nothing, so it needs no
lock at all, which is exactly the reasoning that lets ``--mode check`` read
them and earn a verdict off them.  These tests pin that:

* a host the record already holds at zero past the escalation threshold
  escalates on a pass that could not take the lock, and the state file is left
  byte-for-byte alone, because the lock holder may be writing it;
* a record below the threshold is still read out, without a verdict;
* a durable maintenance park escalates on the same clock as a stranding;
* a fleet with nothing recorded reports the busy lock and nothing else;
* a record for a host outside this declaration escalates nothing, since
  pruning it is a write this pass cannot make;
* damaged state still reports the busy lock instead of raising out of the
  handler that reports it.
"""

from __future__ import annotations

import contextlib
import io
import json
import tempfile
import time
from collections.abc import Callable, Iterator
from pathlib import Path
from types import ModuleType
from typing import Any

PARKED_SECONDS = 600
UNMANAGED_PASSES = 9
UNMANAGED_HOST = "retired-runner"


@contextlib.contextmanager
def _busy_lock(controller: ModuleType) -> Iterator[None]:
    """Hold the fleet mutation lock against this pass, the way another controller does."""
    lock_module = controller.fml
    original = lock_module.mutation_lock

    def refuse(*_args: object, **_kwargs: object) -> None:
        message = "another controller owns the dev-host fleet mutation lock"
        raise lock_module.MutationLockBusyError(message)

    lock_module.mutation_lock = refuse
    try:
        yield
    finally:
        lock_module.mutation_lock = original


def _order(controller: ModuleType) -> list[str]:
    """Return this fleet's capacity-managed hosts, producer first."""
    return controller.runner_hosts(controller.fm.load())


def _record(passes: int) -> dict[str, int]:
    """Return one record of a host held at zero for that many consecutive passes."""
    return {"since": int(time.time()) - PARKED_SECONDS, "passes": passes}


def _document(
    stranded: dict[str, dict[str, int]] | None = None,
    parked: dict[str, dict[str, int]] | None = None,
) -> dict[str, Any]:
    """Return state carrying only the zero-capacity records under test."""
    return {
        "version": 1,
        "hosts": {},
        "stranded": stranded or {},
        "parked": parked or {},
    }


def _seed(controller: ModuleType, state_dir: Path, document: object) -> Path:
    """Write the state one locked-out pass will read, returning its path."""
    state_path = state_dir / controller.STATE_FILE
    encoded = (
        document
        if isinstance(document, str)
        else json.dumps(document, indent=2, sort_keys=True) + "\n"
    )
    state_path.write_text(encoded, encoding="ascii")
    return state_path


def _run_locked(controller: ModuleType, state_dir: Path, document: object) -> tuple[int, str, bool]:
    """Run one apply pass whose mutation lock is already held by somebody else."""
    state_path = _seed(controller, state_dir, document)
    before = state_path.read_bytes()
    stream = io.StringIO()
    with (
        _busy_lock(controller),
        contextlib.redirect_stdout(stream),
        contextlib.redirect_stderr(stream),
    ):
        status = controller.main(["--mode", "apply", "--state-dir", str(state_dir)])
    return status, stream.getvalue(), state_path.read_bytes() == before


def _case_recorded_zero_escalates(
    controller: ModuleType, state_dir: Path, failures: list[str]
) -> None:
    """A host held at zero past the threshold escalates even when the lock is held."""
    producer = _order(controller)[0]
    status, evidence, unchanged = _run_locked(
        controller,
        state_dir,
        _document(stranded={producer: _record(controller.STRANDED_ESCALATION_PASSES)}),
    )
    if status != controller.STRANDED_STATUS:
        failures.append(
            f"locked: a lock-busy pass over a host held at ZERO exited {status}, "
            "expected STRANDED_STATUS"
        )
    if producer not in evidence or "CRITICAL" not in evidence:
        failures.append(
            "locked: a lock-busy pass said nothing critical about the host it is "
            "holding at ZERO capacity"
        )
    if not unchanged:
        failures.append("locked: a lock-busy pass wrote to state the lock holder owns")


def _case_below_threshold_is_read_out(
    controller: ModuleType, state_dir: Path, failures: list[str]
) -> None:
    """A record below the escalation threshold is named without earning a verdict."""
    producer = _order(controller)[0]
    status, evidence, unchanged = _run_locked(
        controller, state_dir, _document(stranded={producer: _record(1)})
    )
    if status != controller.fml.LOCK_BUSY_STATUS:
        failures.append(
            f"locked: one recorded pass at zero exited {status}, expected the busy lock"
        )
    if producer not in evidence:
        failures.append("locked: a lock-busy pass never named the host recorded at ZERO")
    if not unchanged:
        failures.append("locked: reading a below-threshold record aged or rewrote it")


def _case_durable_park_escalates(
    controller: ModuleType, state_dir: Path, failures: list[str]
) -> None:
    """A durable maintenance park escalates on the same clock as a stranding."""
    consumer = _order(controller)[1]
    status, evidence, unchanged = _run_locked(
        controller,
        state_dir,
        _document(parked={consumer: _record(controller.PARK_ESCALATION_PASSES)}),
    )
    if status != controller.STRANDED_STATUS:
        failures.append(
            f"locked: a lock-busy pass over a parked host exited {status}, expected STRANDED_STATUS"
        )
    if consumer not in evidence:
        failures.append("locked: a lock-busy pass never named the host held by a durable park")
    if not unchanged:
        failures.append("locked: reading the parked record aged or rewrote it")


def _case_healthy_fleet_reports_the_lock_alone(
    controller: ModuleType, state_dir: Path, failures: list[str]
) -> None:
    """A fleet with nothing recorded at zero reports the busy lock and nothing more."""
    status, evidence, unchanged = _run_locked(controller, state_dir, _document())
    if status != controller.fml.LOCK_BUSY_STATUS:
        failures.append(
            f"locked: a healthy fleet behind a busy lock exited {status}, expected the busy lock"
        )
    if "ZERO capacity" in evidence:
        failures.append("locked: a healthy fleet was reported as holding zero capacity")
    if not unchanged:
        failures.append("locked: a healthy lock-busy pass wrote to state")


def _case_unmanaged_record_is_not_escalated(
    controller: ModuleType, state_dir: Path, failures: list[str]
) -> None:
    """A record for a host this fleet no longer declares cannot hold the verdict red."""
    status, _evidence, unchanged = _run_locked(
        controller, state_dir, _document(stranded={UNMANAGED_HOST: _record(UNMANAGED_PASSES)})
    )
    if status != controller.fml.LOCK_BUSY_STATUS:
        failures.append(
            f"locked: a retired host's record exited {status} behind a busy lock, "
            "expected the busy lock"
        )
    if not unchanged:
        failures.append("locked: a lock-busy pass pruned a record it cannot persist")


def _case_damaged_state_reports_the_lock(
    controller: ModuleType, state_dir: Path, failures: list[str]
) -> None:
    """Damaged state reports the busy lock rather than raising out of the report."""
    try:
        status, evidence, _unchanged = _run_locked(controller, state_dir, "{not json at all")
    except (OSError, TypeError, ValueError) as error:
        failures.append(f"locked: damaged state raised out of the busy-lock report ({error})")
        return
    if status != controller.fml.LOCK_BUSY_STATUS:
        failures.append(
            f"locked: damaged state behind a busy lock exited {status}, expected the busy lock"
        )
    if "WARNING" not in evidence:
        failures.append("locked: damaged state was read behind a busy lock without a warning")


Case = Callable[[ModuleType, Path, list[str]], None]

CASES: tuple[Case, ...] = (
    _case_recorded_zero_escalates,
    _case_below_threshold_is_read_out,
    _case_durable_park_escalates,
    _case_healthy_fleet_reports_the_lock_alone,
    _case_unmanaged_record_is_not_escalated,
    _case_damaged_state_reports_the_lock,
)


def run(controller: ModuleType) -> list[str]:
    """Run every locked-out reporting case against the controller under test."""
    failures: list[str] = []
    for case in CASES:
        with tempfile.TemporaryDirectory(prefix="ra8-fleet-locked-") as raw:
            case(controller, Path(raw), failures)
    return failures
