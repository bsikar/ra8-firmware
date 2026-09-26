# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Regression selftests for a pass that dies FATAL and reports no zero capacity (#888).

A pass can fail outright once it is under way: the mutation authority lost
rather than merely held by somebody else, the state file unreadable underneath
it, a write to the state directory out of space, a fleet helper it shells out
to gone.  Every one of those ends in ``FATAL`` and status 2, and 2 is what
``--force`` outside apply mode and a negative convergence interval return: in
this controller's own vocabulary it means the operator has asked for something
impossible, go and fix the invocation.

Over a fleet this controller has already taken to ZERO capacity it says the
wrong thing entirely, and it is the same silence the busy lock used to report
before ``locked_out_verdict``.  Both zero-capacity records live in the state
file across passes precisely so a fleet held at zero gets LOUDER rather than
quieter, and they are on disk before this pass even starts: a fault that keeps
arriving at the same point hid every one of them behind what reads like a typo
in the unit file for as long as it lasted, named no host, logged no CRITICAL,
and escalated nothing however many passes it repeated.

Reading those records persists nothing and mutates nothing, which is what lets
``--mode check`` read them and earn a verdict off them, so it is safe on a path
where the pass has already proven it cannot be trusted to write.  These tests
pin that:

* a stranded-at-zero record past the escalation threshold escalates on a pass
  the mutation authority failed under, and the state file is left byte-for-byte
  alone, because this pass died partway through its own transaction;
* a durable maintenance park escalates on the same clock, under a plain
  ``OSError``, so the verdict is not keyed to one exception type;
* a record below the threshold is still read out, without a verdict;
* a fleet with nothing recorded reports the failure and nothing else;
* a record for a host outside this declaration escalates nothing, since
  pruning it is a write this pass cannot make;
* damaged state reports the failure instead of raising out of the handler;
* a failure BEFORE the declaration loads still reports status 2, which is what
  splitting ``open_pass`` out of ``main`` makes structural: that path has no
  declaration to scope the records against, so it must not try.
"""

from __future__ import annotations

import contextlib
import io
import json
import tempfile
import time
from collections.abc import Iterator
from pathlib import Path
from types import ModuleType
from typing import Any

RECORDED_SECONDS = 600
BELOW_THRESHOLD_PASSES = 1
UNMANAGED_PASSES = 9
UNMANAGED_HOST = "retired-runner"
FATAL_STATUS = 2


@contextlib.contextmanager
def _failing_lock(controller: ModuleType, error: BaseException) -> Iterator[None]:
    """Fail this pass's mutation authority the way a lost guardian does."""
    lock_module = controller.fml
    original = lock_module.mutation_lock

    def fail(*_args: object, **_kwargs: object) -> None:
        raise error

    lock_module.mutation_lock = fail
    try:
        yield
    finally:
        lock_module.mutation_lock = original


def _order(controller: ModuleType) -> list[str]:
    """Return this fleet's capacity-managed hosts, producer first."""
    return controller.runner_hosts(controller.fm.load())


def _record(passes: int) -> dict[str, int]:
    """Return one record of a host held at zero for that many consecutive passes."""
    return {"since": int(time.time()) - RECORDED_SECONDS, "passes": passes}


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


def _run_aborted(
    controller: ModuleType,
    state_dir: Path,
    document: object,
    error: BaseException | None = None,
) -> tuple[int, str, bool]:
    """Run one apply pass whose transaction dies FATAL, returning what it reported."""
    failure = error or controller.fml.MutationLockError("mutation guardian lost authority")
    state_path = state_dir / controller.STATE_FILE
    encoded = (
        document
        if isinstance(document, str)
        else json.dumps(document, indent=2, sort_keys=True) + "\n"
    )
    state_path.write_text(encoded, encoding="ascii")
    before = state_path.read_bytes()
    stream = io.StringIO()
    with (
        _failing_lock(controller, failure),
        contextlib.redirect_stdout(stream),
        contextlib.redirect_stderr(stream),
    ):
        status = controller.main(["--mode", "apply", "--state-dir", str(state_dir)])
    return status, stream.getvalue(), state_path.read_bytes() == before


def _case_recorded_zero_escalates(
    controller: ModuleType, state_dir: Path, failures: list[str]
) -> None:
    """A host recorded at zero past the threshold escalates on a pass that died."""
    order = _order(controller)
    host = order[1]
    passes = controller.STRANDED_ESCALATION_PASSES + 1
    status, output, unchanged = _run_aborted(
        controller, state_dir, _document(stranded={host: _record(passes)})
    )
    if status != controller.STRANDED_STATUS:
        failures.append(
            "fleet_reconcile_aborted: a pass that died FATAL over a host held at zero "
            f"returned {status}, not STRANDED_STATUS {controller.STRANDED_STATUS}"
        )
    if host not in output or "CRITICAL" not in output:
        failures.append(
            "fleet_reconcile_aborted: a pass that died FATAL did not name the host it is "
            f"holding at zero: {output!r}"
        )
    if "FATAL" not in output:
        failures.append(
            "fleet_reconcile_aborted: the zero-capacity verdict lost the FATAL that ended "
            f"the pass: {output!r}"
        )
    if not unchanged:
        failures.append(
            "fleet_reconcile_aborted: a pass that died partway through its transaction "
            "rewrote the state file"
        )


def _case_durable_park_escalates(
    controller: ModuleType, state_dir: Path, failures: list[str]
) -> None:
    """A durable park escalates on the same clock, under any failure this pass reports."""
    order = _order(controller)
    host = order[0]
    passes = controller.PARK_ESCALATION_PASSES + 1
    status, output, unchanged = _run_aborted(
        controller,
        state_dir,
        _document(parked={host: _record(passes)}),
        error=OSError("[Errno 28] No space left on device"),
    )
    if status != controller.STRANDED_STATUS:
        failures.append(
            "fleet_reconcile_aborted: a durable park past its threshold returned "
            f"{status} on a pass that died of an OSError, not STRANDED_STATUS "
            f"{controller.STRANDED_STATUS}"
        )
    if host not in output or "durable maintenance park" not in output:
        failures.append(
            "fleet_reconcile_aborted: a pass that died FATAL did not read out the durable "
            f"park holding a host at zero: {output!r}"
        )
    if not unchanged:
        failures.append("fleet_reconcile_aborted: reading the park record rewrote the state file")


def _case_below_threshold_named_without_verdict(
    controller: ModuleType, state_dir: Path, failures: list[str]
) -> None:
    """A record short of the threshold is read out, and earns no verdict."""
    order = _order(controller)
    host = order[1]
    status, output, unchanged = _run_aborted(
        controller, state_dir, _document(stranded={host: _record(BELOW_THRESHOLD_PASSES)})
    )
    if status != FATAL_STATUS:
        failures.append(
            "fleet_reconcile_aborted: a record short of the escalation threshold earned "
            f"verdict {status} instead of leaving the pass's own failure at {FATAL_STATUS}"
        )
    if host not in output:
        failures.append(
            "fleet_reconcile_aborted: a below-threshold record went unreported on a pass "
            f"that died FATAL: {output!r}"
        )
    if not unchanged:
        failures.append(
            "fleet_reconcile_aborted: reading a below-threshold record rewrote the state file"
        )


def _case_healthy_fleet_reports_only_the_failure(
    controller: ModuleType, state_dir: Path, failures: list[str]
) -> None:
    """With nothing recorded, the pass reports its failure and says nothing about capacity."""
    status, output, unchanged = _run_aborted(controller, state_dir, _document())
    if status != FATAL_STATUS:
        failures.append(
            "fleet_reconcile_aborted: a pass that died over a fleet with nothing recorded "
            f"returned {status} instead of {FATAL_STATUS}"
        )
    if "CRITICAL" in output or "ZERO" in output:
        failures.append(
            "fleet_reconcile_aborted: a pass that died over a healthy fleet claimed zero "
            f"capacity: {output!r}"
        )
    if "FATAL" not in output or not unchanged:
        failures.append(
            "fleet_reconcile_aborted: a pass that died over a healthy fleet either lost "
            f"its FATAL or rewrote the state file: {output!r}"
        )


def _case_unmanaged_record_escalates_nothing(
    controller: ModuleType, state_dir: Path, failures: list[str]
) -> None:
    """A record for a host outside this declaration escalates nothing, since pruning is a write."""
    status, output, unchanged = _run_aborted(
        controller,
        state_dir,
        _document(stranded={UNMANAGED_HOST: _record(UNMANAGED_PASSES)}),
    )
    if status != FATAL_STATUS:
        failures.append(
            "fleet_reconcile_aborted: a record for a host this fleet no longer manages "
            f"earned verdict {status} on a pass that cannot prune it"
        )
    if UNMANAGED_HOST in output:
        failures.append(
            "fleet_reconcile_aborted: a pass that died FATAL claimed a host outside its "
            f"declaration: {output!r}"
        )
    if not unchanged:
        failures.append(
            "fleet_reconcile_aborted: a pass that cannot write pruned an unmanaged record"
        )


def _case_damaged_state_reports_the_failure(
    controller: ModuleType, state_dir: Path, failures: list[str]
) -> None:
    """Unreadable state leaves the pass's own failure as all it can honestly report."""
    status, output, _unchanged = _run_aborted(controller, state_dir, "{ this is not json")
    if status != FATAL_STATUS:
        failures.append(
            "fleet_reconcile_aborted: damaged state returned "
            f"{status} instead of the pass's own failure {FATAL_STATUS}"
        )
    if "WARNING" not in output or "FATAL" not in output:
        failures.append(
            "fleet_reconcile_aborted: damaged state was not reported alongside the failure "
            f"that ended the pass: {output!r}"
        )


def _case_failure_before_the_declaration(
    controller: ModuleType, state_dir: Path, failures: list[str]
) -> None:
    """A pass that never loaded a declaration reports 2 and does not reach for the records."""
    original = controller.fm.load

    def unreadable() -> dict[str, Any]:
        message = "dev-host declaration has no 'hosts:' mapping"
        raise controller.fm.FleetError(message)

    controller.fm.load = unreadable
    stream = io.StringIO()
    try:
        with contextlib.redirect_stdout(stream), contextlib.redirect_stderr(stream):
            status = controller.main(["--mode", "apply", "--state-dir", str(state_dir)])
    except Exception as error:  # noqa: BLE001  # the pin is that nothing escapes
        failures.append(
            "fleet_reconcile_aborted: a pass that never loaded a declaration raised "
            f"{error!r} out of main instead of reporting a failure"
        )
        return
    finally:
        controller.fm.load = original
    output = stream.getvalue()
    if status != FATAL_STATUS:
        failures.append(
            "fleet_reconcile_aborted: a pass that never loaded a declaration returned "
            f"{status} instead of {FATAL_STATUS}"
        )
    if "CRITICAL" in output:
        failures.append(
            "fleet_reconcile_aborted: a pass with no declaration to scope the records "
            f"against claimed zero capacity anyway: {output!r}"
        )


def run(controller: ModuleType) -> list[str]:
    """Pin that a pass dying FATAL still reports the capacity it is holding at zero."""
    failures: list[str] = []
    for case in (
        _case_recorded_zero_escalates,
        _case_durable_park_escalates,
        _case_below_threshold_named_without_verdict,
        _case_healthy_fleet_reports_only_the_failure,
        _case_unmanaged_record_escalates_nothing,
        _case_damaged_state_reports_the_failure,
        _case_failure_before_the_declaration,
    ):
        with tempfile.TemporaryDirectory() as raw:
            state_dir = Path(raw) / "state"
            state_dir.mkdir(mode=0o700)
            case(controller, state_dir, failures)
    return failures
