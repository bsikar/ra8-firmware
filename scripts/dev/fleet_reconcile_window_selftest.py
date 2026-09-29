# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Regression selftests for a park release that puts no capacity back (#888).

``capacity-restore`` is the one verb that lifts a durable maintenance park, and
it converges live admission to the host's CURRENT window target, printing the
number it converged to.  That number is ZERO inside a declared quiet-hours
window and for a scale set declared at zero instances.  Every other reopen in
this controller already reads it and refuses to call a reopen to ZERO instances
a recovery: both recovery arms do, and ``TransactionCapacity.served`` does for
the transaction that succeeds.  The park release read it for its warning alone
and threw it away, so a restore that landed cleanly while putting NOTHING back
in service still dropped the park record as lifted.

Dropping that record is what made the pass silent rather than merely wrong.  A
refused drain deliberately writes no stranded-at-zero record, so the park
record is the only one such a host has: once it is gone nothing escalates the
host, nothing counts its capacity as forfeit, the pass drain budget counts it
among the hosts still serving, and the receipt the same pass published keeps
the next pass from looking again for a whole full-apply interval.  A fleet
declared for its runners while serving none of them, at exit 0, is issue #888's
own dry-run evidence.

These tests pin the fix and every edge it must not take with it:

* a release whose restore reports ZERO instances keeps the record, escalates,
  and earns ``STRANDED_STATUS`` instead of exiting 0;
* the held record still counts the host's capacity as forfeit rather than
  serving, which is what the pass drain budget is measured against;
* a release whose restore reports real instances still clears the record;
* a restore from an older host-local script, which says nothing about its
  target, still clears it: nothing is claimed in either direction;
* the ARC opener's own restore is read the same way, since a re-declared scale
  set can still be declared at zero;
* the next pass retires the record on the marker-absent refusal, so a host
  whose window later raises it cannot stay red for good.
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
CONSUMER = "consumer-a"
FIRST_PASS = 1000
PASS_INTERVAL = 100
NOW = FIRST_PASS + PASS_INTERVAL
FULL_INTERVAL = 100
PRODUCER_INTERVAL = 50
DIGEST = "a" * 64
ZERO_RESTORE = "restoring current window target 0\n"
SERVING_RESTORE = "restoring current window target 2\n"
SILENT_RESTORE = "capacity restored\n"
MARKER_ABSENT_STATUS = 1
MARKER_ABSENT = "fleet-capacity: error: cannot restore without a durable maintenance marker\n"
EARLIER_PARK = {"since": FIRST_PASS - 500, "passes": 1}
HELD_PARK = {"since": FIRST_PASS - 500, "passes": 2}


def _data(*, arc: bool = False) -> dict[str, Any]:
    """Return a two-host fleet whose consumer is docker or an ARC scale set."""
    return {
        "runner_image": {"source_host": PRODUCER},
        "hosts": {
            PRODUCER: {
                "class": "docker_linux",
                "runners": {"instances": 2},
                "provisions": ["one", "two"],
            },
            CONSUMER: {
                "class": "arc_k8s" if arc else "docker_linux",
                "runners": {"instances": 2},
                "provisions": ["one"],
            },
        },
    }


def _recap(host: str, changed: int = 0) -> str:
    """Build one successful Ansible recap row."""
    return f"{host} : ok=9 changed={changed} unreachable=0 failed=0 skipped=1 rescued=0 ignored=0\n"


def _clean(controller: ModuleType, data: dict[str, Any], host: str) -> frp.CommandResult:
    """Return the exact accepted check result for one host."""
    noise = controller.PRODUCER_CHECK_NOISE if host == PRODUCER else 0
    rows = [_recap(host, noise)]
    rows.extend(_recap(host) for _ in data["hosts"][host]["provisions"][1:])
    return frp.CommandResult(0, "".join(rows), "")


def _converged_state(controller: ModuleType, state_dir: Path, parked: dict[str, Any]) -> None:
    """Write state whose hosts are converged, with the given durable parks."""
    fresh = {"source_digest": DIGEST, "full_applied_at": NOW - 10, "checked_at": NOW - 10}
    controller.save_state(
        state_dir / controller.STATE_FILE,
        {
            "version": 1,
            "hosts": {PRODUCER: dict(fresh), CONSUMER: dict(fresh)},
            "stranded": {},
            "parked": parked,
        },
    )


def _pass(  # noqa: PLR0913  # one pass's fleet, its state and the restore it turns on
    controller: ModuleType,
    data: dict[str, Any],
    state_dir: Path,
    restore: frp.CommandResult,
    *,
    parked: dict[str, Any] | None = None,
    now: int = NOW,
) -> tuple[int, list[tuple[str, str]], dict[str, Any], str]:
    """Run one apply pass over a converged, parked fleet, recording its verbs."""
    _converged_state(controller, state_dir, parked or {CONSUMER: dict(EARLIER_PARK)})
    calls: list[tuple[str, str]] = []
    options = controller.ReconcileOptions(
        mode="apply",
        force=False,
        source_digest=DIGEST,
        state_dir=state_dir,
        full_interval=FULL_INTERVAL,
        producer_interval=PRODUCER_INTERVAL,
        now=now,
    )

    def run(argv: Sequence[str]) -> frp.CommandResult:
        verb, host = controller._command_identity(argv)  # noqa: SLF001
        calls.append((verb, host))
        if verb == "restore":
            return restore
        if verb in {"check", "parked-check"}:
            return _clean(controller, data, host)
        return frp.CommandResult(0, "", "")

    log = io.StringIO()
    with contextlib.redirect_stderr(log), contextlib.redirect_stdout(io.StringIO()):
        status = controller.reconcile(data, options, run, controller._no_wait)  # noqa: SLF001
    stored = controller.load_state(state_dir / controller.STATE_FILE)
    return status, calls, stored, log.getvalue()


def _zero_release_is_not_a_release(controller: ModuleType, failures: list[str]) -> None:
    """A restore that put nothing in service keeps the record and carries a verdict."""
    data = _data()
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-window-") as raw:
        state_dir = Path(raw)
        status, calls, stored, log = _pass(
            controller, data, state_dir, frp.CommandResult(0, ZERO_RESTORE, "")
        )
        if status != controller.STRANDED_STATUS:
            failures.append(
                f"window: a park release that put ZERO instances back returned {status}, "
                f"not STRANDED_STATUS ({controller.STRANDED_STATUS})"
            )
        if ("restore", CONSUMER) not in calls:
            failures.append(f"window: the release never issued the restore: {calls}")
        if stored.get("parked") != {CONSUMER: HELD_PARK}:
            failures.append(
                "window: a restore that put ZERO instances back still dropped the park "
                f"record: {stored.get('parked')}"
            )
        if "CRITICAL" not in log or CONSUMER not in log:
            failures.append("window: a release that reopened nothing was not escalated by name")


def _held_record_still_forfeits_capacity(controller: ModuleType, failures: list[str]) -> None:
    """The held record keeps the host out of the capacity the drain budget may spend."""
    data = _data()
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-window-") as raw:
        state_dir = Path(raw)
        _status, _calls, stored, _log = _pass(
            controller, data, state_dir, frp.CommandResult(0, ZERO_RESTORE, "")
        )
        order = [PRODUCER, CONSUMER]
        serving = controller.serving_hosts(order, stored.get("stranded") or {}, stored["parked"])
        if CONSUMER in serving:
            failures.append(
                f"window: a host held at zero by its park was counted as serving: {serving}"
            )


def _serving_release_still_clears(controller: ModuleType, failures: list[str]) -> None:
    """A restore that reported real instances still lifts the park."""
    data = _data()
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-window-") as raw:
        state_dir = Path(raw)
        status, _calls, stored, _log = _pass(
            controller, data, state_dir, frp.CommandResult(0, SERVING_RESTORE, "")
        )
        if status:
            failures.append(f"window: a landed release failed the pass ({status})")
        if stored.get("parked"):
            failures.append(f"window: a landed release left the park recorded: {stored['parked']}")


def _silent_restore_still_clears(controller: ModuleType, failures: list[str]) -> None:
    """An older host-local restore claims nothing, so the reopen verb stands alone."""
    data = _data()
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-window-") as raw:
        state_dir = Path(raw)
        status, _calls, stored, _log = _pass(
            controller, data, state_dir, frp.CommandResult(0, SILENT_RESTORE, "")
        )
        if status:
            failures.append(f"window: a restore that said nothing failed the pass ({status})")
        if stored.get("parked"):
            failures.append(
                "window: a restore that reported no target at all was read as putting zero "
                f"back: {stored['parked']}"
            )


def _arc_release_is_read_the_same_way(controller: ModuleType, failures: list[str]) -> None:
    """A re-declared scale set restored to zero instances is not released either."""
    data = _data(arc=True)
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-window-") as raw:
        state_dir = Path(raw)
        status, calls, stored, _log = _pass(
            controller, data, state_dir, frp.CommandResult(0, ZERO_RESTORE, "")
        )
        if ("activate", CONSUMER) not in calls:
            failures.append(f"window: the ARC release skipped its own opener: {calls}")
        if status != controller.STRANDED_STATUS:
            failures.append(f"window: an ARC release that reopened nothing returned {status}")
        if stored.get("parked") != {CONSUMER: HELD_PARK}:
            failures.append(
                f"window: an ARC restore to zero dropped the park record: {stored.get('parked')}"
            )


def _next_pass_retires_a_lifted_marker(controller: ModuleType, failures: list[str]) -> None:
    """The held record is retired once the restore reports the marker is gone."""
    data = _data()
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-window-") as raw:
        state_dir = Path(raw)
        status, _calls, stored, _log = _pass(
            controller,
            data,
            state_dir,
            frp.CommandResult(MARKER_ABSENT_STATUS, "", MARKER_ABSENT),
            parked={CONSUMER: dict(HELD_PARK)},
        )
        if status:
            failures.append(f"window: an already-lifted marker failed the later pass ({status})")
        if stored.get("parked"):
            failures.append(f"window: the held record outlived its own marker: {stored['parked']}")


def _unit_level_read(controller: ModuleType, failures: list[str]) -> None:
    """Prove the one read this decision turns on, in both directions."""
    zero = controller.restore_admission(CONSUMER, frp.CommandResult(0, ZERO_RESTORE, ""))
    if zero != 0:
        failures.append(f"window: a restore to zero instances read back as {zero}")
    serving = controller.restore_admission(CONSUMER, frp.CommandResult(0, SERVING_RESTORE, ""))
    if serving == 0:
        failures.append("window: a restore to two instances read back as zero")
    silent = controller.restore_admission(CONSUMER, frp.CommandResult(0, SILENT_RESTORE, ""))
    if silent is not None:
        failures.append(f"window: a restore that claimed no target read back as {silent}")


def run(controller: ModuleType) -> list[str]:
    """Run every zero-capacity park-release regression case."""
    failures: list[str] = []
    _zero_release_is_not_a_release(controller, failures)
    _held_record_still_forfeits_capacity(controller, failures)
    _serving_release_still_clears(controller, failures)
    _silent_restore_still_clears(controller, failures)
    _arc_release_is_read_the_same_way(controller, failures)
    _next_pass_retires_a_lifted_marker(controller, failures)
    _unit_level_read(controller, failures)
    return failures
