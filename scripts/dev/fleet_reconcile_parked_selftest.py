# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Regression selftests for the durable park a refused drain leaves behind (#888).

``capacity-quarantine`` writes the host's DURABLE maintenance marker before it
attempts the drain (``park_maintenance`` is ``set_maintenance`` and only then
the drain), so a drain the fleet entry point REFUSES still leaves the marker on
the host.  With that marker in place the host-local window timer refuses to
raise admission again ("maintenance: forcing target 0") and only
``capacity-restore`` removes it.  A refused drain is therefore not a host left
serving indefinitely: it is a host left PARKED and heading to zero on its own
timer.

The controller remembered nothing about it.  Writing the stranded-at-zero
record there would forge the claim that this controller took the host to zero
and proved it, which three later policies read back, so the refused drain
deliberately recorded nothing at all.  That left the host invisible: no
escalation counted it, the dry run had nothing to report, and the pass drain
budget counted it as capacity still serving.  Worst of all, a later pass whose
budget was already spent HELD it, so nothing ever reached the host to issue the
one restore that could lift its park.

These tests pin a separate record that claims only what the pass proved:

* a refused drain records the park, and still records no zero capacity;
* the pass drain budget never holds a parked host back, so a later pass
  reaches it, reopens it, and the park record is cleared by that reopen;
* a read-only dry run names every parked host and persists nothing;
* only a landed reopen verb clears the record, because the restore is what
  removes the marker;
* the parked record is read by nothing else: the frozen-image release, the
  producer block delay and the serving-host count are all unchanged by it.
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
CONSUMER_B = "consumer-b"
DRAIN_REFUSED_STATUS = 7
APPLY_FAILURE_STATUS = 1
ORDINARY_FAILURE_STATUS = 1
FIRST_PASS = 1000
PASS_INTERVAL = 100
FULL_INTERVAL = 100
PRODUCER_INTERVAL = 50
DIGEST = "a" * 64
RESTORE_OUTPUT = "restoring current window target 2\n"
EARLIER_PARK = {"since": 500, "passes": 1}


def _data() -> dict[str, Any]:
    """Return a three-host fleet whose drain budget allows exactly one host."""
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
            CONSUMER_B: {
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
    controller: ModuleType, state_dir: Path, *, mode: str = "apply", now: int = FIRST_PASS
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


def _refused_drain_records_the_park(controller: ModuleType, failures: list[str]) -> None:
    """A refused drain records the durable park, and still records no zero capacity."""
    data = _data()
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-parked-") as raw:
        state_dir = Path(raw)
        options = _options(controller, state_dir)

        def run(argv: Sequence[str]) -> frp.CommandResult:
            verb, host = controller._command_identity(argv)  # noqa: SLF001
            if verb in {"check", "parked-check"}:
                if host == CONSUMER_A and verb == "check":
                    return _check(data, host, 1)
                return _clean(controller, data, host)
            if verb == "parked-apply" and host == CONSUMER_A:
                return frp.CommandResult(APPLY_FAILURE_STATUS, "", "")
            if verb == "quarantine":
                return frp.CommandResult(DRAIN_REFUSED_STATUS, "", "")
            if verb == "restore":
                return frp.CommandResult(0, RESTORE_OUTPUT, "")
            return frp.CommandResult(0, "", "")

        status = controller.reconcile(data, options, run, controller._no_wait)  # noqa: SLF001
        if status != controller.DRAIN_FAILED_STATUS:
            failures.append(f"parked: refused drain returned {status}, not the loudest verdict")
        stored = controller.load_state(state_dir / controller.STATE_FILE)
        if stored.get("parked") != {CONSUMER_A: {"since": FIRST_PASS, "passes": 1}}:
            failures.append(
                f"parked: refused drain recorded no durable park: {stored.get('parked')}"
            )
        if stored.get("stranded"):
            failures.append(
                f"parked: refused drain forged a zero-capacity record: {stored['stranded']}"
            )
        if CONSUMER_A in stored["hosts"]:
            failures.append("parked: refused drain kept the host's receipt")


def _budget_never_holds_a_parked_host(controller: ModuleType, failures: list[str]) -> None:
    """A pass whose budget is spent still reaches a parked host, and the reopen clears it."""
    data = _data()
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-parked-") as raw:
        state_dir = Path(raw)
        options = _options(controller, state_dir, now=FIRST_PASS + PASS_INTERVAL)
        fresh = {"source_digest": DIGEST, "full_applied_at": FIRST_PASS + PASS_INTERVAL - 10}
        controller.save_state(
            state_dir / controller.STATE_FILE,
            {
                "version": 1,
                "hosts": {PRODUCER: fresh},
                "stranded": {},
                "parked": {CONSUMER_B: dict(EARLIER_PARK)},
            },
        )
        calls: list[tuple[str, str]] = []

        def run(argv: Sequence[str]) -> frp.CommandResult:
            verb, host = controller._command_identity(argv)  # noqa: SLF001
            calls.append((verb, host))
            if verb in {"check", "parked-check"}:
                if host == CONSUMER_A and verb == "check":
                    return _check(data, host, 1)
                return _clean(controller, data, host)
            if verb == "parked-apply" and host == CONSUMER_A:
                return frp.CommandResult(APPLY_FAILURE_STATUS, "", "")
            if verb == "restore":
                return frp.CommandResult(0, RESTORE_OUTPUT, "")
            return frp.CommandResult(0, "", "")

        status = controller.reconcile(data, options, run, controller._no_wait)  # noqa: SLF001
        if ("parked-apply", CONSUMER_B) not in calls or ("restore", CONSUMER_B) not in calls:
            failures.append(f"parked: the spent drain budget held a parked host back: {calls}")
        stored = controller.load_state(state_dir / controller.STATE_FILE)
        if stored.get("parked"):
            failures.append(f"parked: a reopened host kept its parked record: {stored['parked']}")
        if stored.get("stranded", {}).get(CONSUMER_A, {}).get("passes") != 1:
            failures.append("parked: the drain that landed stopped recording zero capacity")
        if status != ORDINARY_FAILURE_STATUS:
            failures.append(f"parked: reaching the parked host changed the verdict to {status}")
        if CONSUMER_B not in stored["hosts"]:
            failures.append("parked: the repaired host published no receipt")


def _dry_run_names_every_parked_host(controller: ModuleType, failures: list[str]) -> None:
    """A read-only pass says which hosts are parked, and writes nothing."""
    data = _data()
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-parked-") as raw:
        state_dir = Path(raw)
        options = _options(controller, state_dir, mode="check", now=FIRST_PASS + PASS_INTERVAL)
        document = {
            "version": 1,
            "hosts": {},
            "stranded": {},
            "parked": {CONSUMER_A: dict(EARLIER_PARK)},
        }
        controller.save_state(state_dir / controller.STATE_FILE, document)

        def run(argv: Sequence[str]) -> frp.CommandResult:
            verb, host = controller._command_identity(argv)  # noqa: SLF001
            if verb != "check":
                failures.append(f"parked: the dry run issued {verb} against {host}")
            return _clean(controller, data, host)

        stream = io.StringIO()
        with contextlib.redirect_stderr(stream):
            status = controller.reconcile(data, options, run, controller._no_wait)  # noqa: SLF001
        if status:
            failures.append(f"parked: a clean dry run over a parked host returned {status}")
        if "durable maintenance park" not in stream.getvalue():
            failures.append("parked: the dry run said nothing about a host it is holding parked")
        stored = controller.load_state(state_dir / controller.STATE_FILE)
        if stored.get("parked") != {CONSUMER_A: dict(EARLIER_PARK)}:
            failures.append(
                f"parked: the dry run rewrote the parked record: {stored.get('parked')}"
            )


def _only_a_reopen_clears_the_record(controller: ModuleType, failures: list[str]) -> None:
    """The restore that removes the marker is the only evidence the park is gone."""
    parked: dict[str, dict[str, int]] = {}
    controller.invalidate_receipt(
        {}, {}, [], CONSUMER_A, FIRST_PASS, stranded=True, at_zero=False, parked=parked
    )
    if parked != {CONSUMER_A: {"since": FIRST_PASS, "passes": 1}}:
        failures.append(f"parked: a refused drain recorded no park: {parked}")
    controller.invalidate_receipt(
        {}, {}, [], CONSUMER_A, FIRST_PASS + PASS_INTERVAL, stranded=False, parked=parked
    )
    if CONSUMER_A not in parked:
        failures.append("parked: a failure that reopened nothing dropped the park record")
    controller.invalidate_receipt(
        {}, {}, [], CONSUMER_A, FIRST_PASS, stranded=False, reopened=True, parked=parked
    )
    if parked:
        failures.append(f"parked: a landed reopen kept the park record: {parked}")
    held = {CONSUMER_B: dict(EARLIER_PARK)}
    controller.record_success(
        {}, {}, [PRODUCER, CONSUMER_B], CONSUMER_B, {}, index=1, released=False, parked=held
    )
    if held != {CONSUMER_B: dict(EARLIER_PARK)}:
        failures.append("parked: a success that issued no reopen claimed the park was lifted")
    controller.record_success(
        {},
        {},
        [PRODUCER, CONSUMER_B],
        CONSUMER_B,
        {},
        index=1,
        released=False,
        reopened=True,
        parked=held,
    )
    if held:
        failures.append(f"parked: a reopened host kept its park record on success: {held}")


def _policy_stays_narrow(controller: ModuleType, failures: list[str]) -> None:
    """Nothing else reads the parked record, and malformed state is replaced."""
    document: dict[str, Any] = {"version": 1, "hosts": {}, "parked": {CONSUMER_A: "nonsense"}}
    if controller.load_parked(document) or document["parked"]:
        failures.append("parked: malformed state was trusted")
    if controller.load_parked({"version": 1, "hosts": {}}) != {}:
        failures.append("parked: older state without the record did not start empty")
    order = [PRODUCER, CONSUMER_A]
    document = {
        "version": 1,
        "hosts": {},
        "parked": {CONSUMER_A: dict(EARLIER_PARK), "retired": dict(EARLIER_PARK)},
    }
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-parked-") as raw:
        options = _options(controller, Path(raw))
        opened = controller.open_parked(document, order, options)
        if "retired" in opened:
            failures.append("parked: a host outside the declaration kept its park record")
        if opened[CONSUMER_A]["passes"] != EARLIER_PARK["passes"] + 1:
            failures.append(f"parked: an apply pass did not count the park: {opened}")
        document["parked"] = {CONSUMER_A: dict(EARLIER_PARK)}
        check_options = _options(controller, Path(raw), mode="check")
        if controller.open_parked(document, order, check_options) != {
            CONSUMER_A: dict(EARLIER_PARK)
        }:
            failures.append("parked: a check pass rewrote the park record")
    parked = {CONSUMER_A: dict(EARLIER_PARK)}
    if controller.serving_hosts(order, {}) != order:
        failures.append("parked: the serving-host count read the park record")
    if controller.frozen_image_release({}, PRODUCER, undrained=False):
        failures.append("parked: a park record froze a producer's image")
    if controller.consumers_released({}, PRODUCER, undrained=False):
        failures.append("parked: a park record spent the producer block delay")
    halted: list[str] = []
    if not controller.consumer_held(
        CONSUMER_A, {}, halted, blocking=True, drained=[], budget=1, parked=parked
    ):
        failures.append("parked: a parked consumer was let past a failed producer")
    if controller.consumer_held(
        CONSUMER_A, {}, halted, blocking=False, drained=[PRODUCER], budget=1, parked=parked
    ):
        failures.append("parked: the drain budget held a parked host back")
    if halted:
        failures.append(f"parked: a parked host was counted as a cascade halt: {halted}")


def run(controller: ModuleType) -> list[str]:
    """Return every durable-park accounting failure."""
    failures: list[str] = []
    _refused_drain_records_the_park(controller, failures)
    _budget_never_holds_a_parked_host(controller, failures)
    _dry_run_names_every_parked_host(controller, failures)
    _only_a_reopen_clears_the_record(controller, failures)
    _policy_stays_narrow(controller, failures)
    return failures
