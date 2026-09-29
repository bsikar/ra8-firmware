# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Regression selftests for lifting a durable maintenance park (#888).

A drain the fleet entry point REFUSES still leaves the host's durable
maintenance marker behind, because ``capacity-quarantine`` writes that marker
before it attempts the drain.  With the marker in place ``cmd_window``, the
host-local timer that raises admission again when a quiet window ends, refuses
to lift it ("maintenance: forcing target 0"), and ``capacity-restore`` is the
only thing that removes it.  The controller learned to REMEMBER that park, and
then nothing ever issued that restore: a later pass whose check came back
CLEAN mutates nothing at all, so it published a receipt, cleared no park, and
exited 0 with the host pinned at zero admission for as long as that receipt
stayed valid, which is a whole full-apply interval -- seven days in
production.  A fleet that cannot climb back out on its own is issue #888
itself, arriving through the newest record.

These tests pin the recovery and its limits:

* a host whose declaration reconciled this pass while still parked is
  reopened with the one verb that lifts the park, and the record is dropped;
* a restore this controller issued and the host REFUSED keeps the record and
  escalates, because the pass has proven it cannot reopen that host itself;
* a park somebody else lifted is dropped on the restore's own refusal rather
  than failing every later pass over a host that is fine;
* a read-only dry run issues nothing and persists nothing;
* only a host whose own declaration verified THIS pass is reopened, and only
  one this fleet still declares.
"""

from __future__ import annotations

import contextlib
import io
import tempfile
from collections.abc import Sequence
from pathlib import Path
from types import ModuleType
from typing import Any

import fleet_reconcile_process as frp

PRODUCER = "producer"
CONSUMER_A = "consumer-a"
RETIRED = "retired-host"
FIRST_PASS = 1000
PASS_INTERVAL = 100
NOW = FIRST_PASS + PASS_INTERVAL
FULL_INTERVAL = 100
PRODUCER_INTERVAL = 50
DIGEST = "a" * 64
RESTORE_OUTPUT = "restoring current window target 2\n"
RESTORE_REFUSED_STATUS = 9
MARKER_ABSENT_STATUS = 1
MARKER_ABSENT = "fleet-capacity: error: cannot restore without a durable maintenance marker\n"
CHECK_FAILURE_STATUS = 2
EARLIER_PARK = {"since": FIRST_PASS - 500, "passes": 1}
AGED_PARK = {"since": FIRST_PASS - 500, "passes": 2}


def _data() -> dict[str, Any]:
    """Return a two-host fleet: the image producer and one consumer."""
    return {
        "runner_image": {"source_host": PRODUCER},
        "hosts": {
            PRODUCER: {
                "class": "docker_linux",
                "runners": {"instances": 2},
                "provisions": ["one", "two"],
            },
            CONSUMER_A: {
                "class": "docker_linux",
                "runners": {"instances": 2},
                "provisions": ["one"],
            },
        },
    }


def _recap(host: str, changed: int = 0) -> str:
    """Build one successful Ansible recap row."""
    return f"{host} : ok=9 changed={changed} unreachable=0 failed=0 skipped=1 rescued=0 ignored=0\n"


def _check(data: dict[str, Any], host: str, changed: int) -> frp.CommandResult:
    """Return a successful check carrying this host's declared recap rows."""
    rows = [_recap(host, changed)]
    rows.extend(_recap(host) for _ in data["hosts"][host]["provisions"][1:])
    return frp.CommandResult(0, "".join(rows), "")


def _clean(controller: ModuleType, data: dict[str, Any], host: str) -> frp.CommandResult:
    """Return the exact accepted check result for one host."""
    noise = controller.PRODUCER_CHECK_NOISE if host == PRODUCER else 0
    return _check(data, host, noise)


def _options(
    controller: ModuleType, state_dir: Path, *, mode: str = "apply", now: int = NOW
) -> object:
    """Return deterministic policy inputs for one pass."""
    return controller.ReconcileOptions(
        mode=mode,
        force=False,
        source_digest=DIGEST,
        state_dir=state_dir,
        full_interval=FULL_INTERVAL,
        producer_interval=PRODUCER_INTERVAL,
        now=now,
    )


def _converged_state(controller: ModuleType, state_dir: Path, parked: dict[str, Any]) -> None:
    """Write state whose hosts are converged, with the given durable parks."""
    fresh = {
        "source_digest": DIGEST,
        "full_applied_at": NOW - 10,
        "checked_at": NOW - 10,
    }
    controller.save_state(
        state_dir / controller.STATE_FILE,
        {
            "version": 1,
            "hosts": {PRODUCER: dict(fresh), CONSUMER_A: dict(fresh)},
            "stranded": {},
            "parked": parked,
        },
    )


def _pass(  # noqa: PLR0913  # one pass's fleet, its state and the two results it turns on
    controller: ModuleType,
    data: dict[str, Any],
    state_dir: Path,
    restore: frp.CommandResult,
    *,
    mode: str = "apply",
    check_status: int = 0,
) -> tuple[int, list[tuple[str, str]], dict[str, Any]]:
    """Run one pass over a converged fleet, recording every verb it issued."""
    calls: list[tuple[str, str]] = []
    options = _options(controller, state_dir, mode=mode)

    def run(argv: Sequence[str]) -> frp.CommandResult:
        verb, host = controller._command_identity(argv)  # noqa: SLF001
        calls.append((verb, host))
        if verb == "restore":
            return restore
        if verb in {"check", "parked-check"}:
            if host == CONSUMER_A and check_status:
                return frp.CommandResult(check_status, "", "")
            return _clean(controller, data, host)
        return frp.CommandResult(0, "", "")

    with contextlib.redirect_stderr(io.StringIO()):
        status = controller.reconcile(data, options, run, controller._no_wait)  # noqa: SLF001
    return status, calls, controller.load_state(state_dir / controller.STATE_FILE)


def _converged_host_still_parked_is_reopened(controller: ModuleType, failures: list[str]) -> None:
    """A host that reconciles while parked is reopened, and its record is dropped."""
    data = _data()
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-lift-") as raw:
        state_dir = Path(raw)
        _converged_state(controller, state_dir, {CONSUMER_A: dict(EARLIER_PARK)})
        status, calls, stored = _pass(
            controller, data, state_dir, frp.CommandResult(0, RESTORE_OUTPUT, "")
        )
        if status:
            failures.append(f"lift: reopening a parked host failed the pass ({status})")
        if ("restore", CONSUMER_A) not in calls:
            failures.append(f"lift: no capacity restore reached the parked host: {calls}")
        if stored.get("parked"):
            failures.append(f"lift: a landed restore left the park recorded: {stored['parked']}")
        if stored["hosts"].get(CONSUMER_A, {}).get("checked_at") != NOW:
            failures.append("lift: the reopened host lost the receipt this pass published")
        if ("restore", PRODUCER) in calls:
            failures.append("lift: an unparked host was reopened anyway")


def _refused_release_escalates(controller: ModuleType, failures: list[str]) -> None:
    """A refused restore keeps the record and earns the stranded verdict."""
    data = _data()
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-lift-") as raw:
        state_dir = Path(raw)
        _converged_state(controller, state_dir, {CONSUMER_A: dict(EARLIER_PARK)})
        status, calls, stored = _pass(
            controller,
            data,
            state_dir,
            frp.CommandResult(RESTORE_REFUSED_STATUS, "", "capacity lock is busy\n"),
        )
        if status != controller.STRANDED_STATUS:
            failures.append(f"lift: a park this pass could not lift returned {status}")
        if ("restore", CONSUMER_A) not in calls:
            failures.append("lift: the refusal case never issued the restore")
        if stored.get("parked") != {CONSUMER_A: AGED_PARK}:
            failures.append(f"lift: a refused restore lost the park record: {stored.get('parked')}")


def _park_lifted_elsewhere_is_dropped(controller: ModuleType, failures: list[str]) -> None:
    """A park somebody else lifted is dropped on the restore's own refusal."""
    data = _data()
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-lift-") as raw:
        state_dir = Path(raw)
        _converged_state(controller, state_dir, {CONSUMER_A: dict(EARLIER_PARK)})
        status, _calls, stored = _pass(
            controller,
            data,
            state_dir,
            frp.CommandResult(MARKER_ABSENT_STATUS, "", MARKER_ABSENT),
        )
        if status:
            failures.append(f"lift: an already-lifted park failed the pass ({status})")
        if stored.get("parked"):
            failures.append(f"lift: an already-lifted park stayed recorded: {stored['parked']}")


def _check_mode_lifts_nothing(controller: ModuleType, failures: list[str]) -> None:
    """A read-only dry run issues no restore and rewrites no record."""
    data = _data()
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-lift-") as raw:
        state_dir = Path(raw)
        _converged_state(controller, state_dir, {CONSUMER_A: dict(EARLIER_PARK)})
        status, calls, stored = _pass(
            controller,
            data,
            state_dir,
            frp.CommandResult(0, RESTORE_OUTPUT, ""),
            mode="check",
        )
        if status:
            failures.append(f"lift: a dry run over a parked host returned {status}")
        if any(verb == "restore" for verb, _host in calls):
            failures.append(f"lift: a read-only pass mutated a host's admission: {calls}")
        if stored.get("parked") != {CONSUMER_A: dict(EARLIER_PARK)}:
            failures.append(f"lift: a dry run rewrote the park record: {stored.get('parked')}")


def _only_a_reconciled_managed_host_is_reopened(
    controller: ModuleType, failures: list[str]
) -> None:
    """A host whose own check failed, and one this fleet retired, are left alone."""
    data = _data()
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-lift-") as raw:
        state_dir = Path(raw)
        _converged_state(
            controller,
            state_dir,
            {CONSUMER_A: dict(EARLIER_PARK), RETIRED: dict(EARLIER_PARK)},
        )
        status, calls, stored = _pass(
            controller,
            data,
            state_dir,
            frp.CommandResult(0, RESTORE_OUTPUT, ""),
            check_status=CHECK_FAILURE_STATUS,
        )
        if status != 1:
            failures.append(f"lift: a failed check over a parked host returned {status}")
        if any(verb == "restore" for verb, _host in calls):
            failures.append(f"lift: a host whose check failed was reopened anyway: {calls}")
        if stored.get("parked") != {CONSUMER_A: AGED_PARK}:
            failures.append(f"lift: the failed pass kept the wrong parks: {stored.get('parked')}")


def _unit_level_reads(controller: ModuleType, failures: list[str]) -> None:
    """Prove both directions of the two reads this recovery turns on."""
    if not controller.park_marker_absent(MARKER_ABSENT):
        failures.append("lift: the restore's own no-marker refusal was not recognised")
    if controller.park_marker_absent("capacity lock is busy"):
        failures.append("lift: an unrelated refusal was read as a lifted park")
    receipts = {
        "fresh": {"checked_at": NOW},
        "stale": {"checked_at": NOW - 1},
        "malformed": "receipt",
    }
    if not controller.reconciled_this_pass(receipts, "fresh", NOW):
        failures.append("lift: a receipt this pass published was not read as reconciled")
    failures.extend(
        f"lift: a {host} receipt was read as reconciled this pass"
        for host in ("stale", "malformed", "absent")
        if controller.reconciled_this_pass(receipts, host, NOW)
    )


def run(controller: ModuleType) -> list[str]:
    """Run every durable-park recovery regression case."""
    failures: list[str] = []
    _converged_host_still_parked_is_reopened(controller, failures)
    _refused_release_escalates(controller, failures)
    _park_lifted_elsewhere_is_dropped(controller, failures)
    _check_mode_lifts_nothing(controller, failures)
    _only_a_reconciled_managed_host_is_reopened(controller, failures)
    _unit_level_reads(controller, failures)
    return failures
