# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Regression selftests for stranding records left by retired hosts (#888).

The stranded-at-zero record survives receipt invalidation on purpose: it is the
only state that can tell one transient failure from a fleet that has been
sitting at zero for days, and both the escalation and the consumer release read
it.  Nothing but the host reconciling ever cleared it, so a runner pulled out of
the declaration while it was stranded, which is exactly what an operator does
with a host that has failed to come back five times, kept its record for good.
Every later pass then escalated a host the controller no longer manages: a
CRITICAL line and ``STRANDED_STATUS`` on a fleet that is entirely healthy, for
as long as the state file lives.  Issue #888 went unnoticed five times because
a fleet at zero read exactly like an ordinary failure, and a verdict that can
never return to zero throws that away again from the other end: the timer is
already red when the next real stranding arrives.

These tests pin the narrow fix:

* a record for a host outside the declaration is dropped, loudly and once, and
  a healthy fleet goes back to exiting clean;
* a host still in the declaration keeps its record and still escalates, even
  when this pass never inspected it;
* a check pass, which persists nothing, reports the record as it stands rather
  than announcing a cleanup it will not make.
"""

from __future__ import annotations

import json
import tempfile
from collections.abc import Sequence
from pathlib import Path
from types import ModuleType
from typing import Any

import fleet_reconcile_process as frp

APPLY_FAILURE_STATUS = 1
FIRST_PASS = 9000
PASS_INTERVAL = 10
FULL_INTERVAL = 1000
PRODUCER_INTERVAL = 500
RETIRED_SINCE = 100
RETIRED_PASSES = 5
DIGEST = "d" * 64

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
    """Return one image producer and one capacity-managed consumer."""
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


def _clean(controller: ModuleType, data: dict[str, Any], host: str) -> frp.CommandResult:
    """Return the exact accepted check result for one converged host."""
    producer = host == data["runner_image"]["source_host"]
    return _check(controller, data, host, controller.PRODUCER_CHECK_NOISE if producer else 0)


def _options(controller: ModuleType, state_dir: Path, now: int, mode: str = "apply") -> object:
    """Return deterministic policy for one pass at ``now``.

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
        now=now,
    )


def _no_wait(_seconds: float) -> None:
    """Take retry pacing out of these cases without skipping it."""


def _seed(controller: ModuleType, state_dir: Path, stranded: dict[str, Any]) -> None:
    """Write state carrying a stranded-at-zero record and no receipts."""
    document = {"version": 1, "hosts": {}, "stranded": stranded}
    (state_dir / controller.STATE_FILE).write_text(json.dumps(document), encoding="ascii")


def _document(controller: ModuleType, state_dir: Path) -> dict[str, Any]:
    """Return the state file as it stands on disk."""
    path = state_dir / controller.STATE_FILE
    return json.loads(path.read_text(encoding="ascii")) if path.exists() else {}


def _stranding(controller: ModuleType, state_dir: Path) -> dict[str, Any]:
    """Return the stranded-at-zero record the state file carries."""
    stored = _document(controller, state_dir).get("stranded")
    return stored if isinstance(stored, dict) else {}


def _retired() -> dict[str, Any]:
    """Return the record left behind by a runner pulled from the fleet."""
    return {"retired-runner": {"since": RETIRED_SINCE, "passes": RETIRED_PASSES}}


def _healthy_pass(
    controller: ModuleType, state_dir: Path, now: int, mode: str = "apply"
) -> tuple[int, list[tuple[str, str]]]:
    """Run one pass in which every managed host converges and serves."""
    data = _data()
    calls: list[tuple[str, str]] = []

    def fake_run(argv: Sequence[str]) -> frp.CommandResult:
        verb, host = _identity(argv)
        calls.append((verb, host))
        if verb in {"check", "parked-check"}:
            return _clean(controller, data, host)
        return frp.CommandResult(0, "", "")

    status = controller.reconcile(
        data, _options(controller, state_dir, now, mode), fake_run, _no_wait
    )
    return status, calls


def _stranded_pass(
    controller: ModuleType, state_dir: Path, now: int
) -> tuple[int, list[tuple[str, str]]]:
    """Run one pass whose drifting producer cannot apply and is drained."""
    data = _data()
    calls: list[tuple[str, str]] = []

    def fake_run(argv: Sequence[str]) -> frp.CommandResult:
        verb, host = _identity(argv)
        calls.append((verb, host))
        if verb in {"check", "parked-check"}:
            drift = 1 if host == "producer" else 0
            base = controller.PRODUCER_CHECK_NOISE if host == "producer" else 0
            return _check(controller, data, host, base + drift)
        if verb == "parked-apply" and host == "producer":
            return frp.CommandResult(APPLY_FAILURE_STATUS, "", "runner image build failed\n")
        return frp.CommandResult(0, "", "")

    status = controller.reconcile(data, _options(controller, state_dir, now), fake_run, _no_wait)
    return status, calls


def _retired_host_stops_failing_a_healthy_fleet(
    controller: ModuleType, failures: list[str]
) -> None:
    """A record for a host outside the declaration may not fail the pass."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-prune-") as raw:
        state_dir = Path(raw)
        _seed(controller, state_dir, _retired())
        status, _calls = _healthy_pass(controller, state_dir, FIRST_PASS)
        document = _document(controller, state_dir)
    if status:
        failures.append(
            f"a pass that reconciled every managed host exited {status} because the state "
            "file still carried a stranding record for a host this fleet no longer manages; "
            "the timer stays red for good and the next real stranding arrives unnoticed"
        )
    if "retired-runner" in document.get("stranded", {}):
        failures.append("the retired host's stranding record survived the pass that reported it")
    if sorted(document.get("hosts", {})) != ["consumer", "producer"]:
        failures.append("pruning the record cost the managed hosts their receipts")


def _second_pass_stays_quiet(controller: ModuleType, failures: list[str]) -> None:
    """The record is reported once, not on every pass for ever."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-prune-") as raw:
        state_dir = Path(raw)
        _seed(controller, state_dir, _retired())
        _healthy_pass(controller, state_dir, FIRST_PASS)
        status, _calls = _healthy_pass(controller, state_dir, FIRST_PASS + PASS_INTERVAL)
        stranding = _stranding(controller, state_dir)
    if status or stranding:
        failures.append(
            f"the pass after the cleanup exited {status} carrying {stranding}, so the record "
            "was never really dropped"
        )


def _managed_host_still_escalates(controller: ModuleType, failures: list[str]) -> None:
    """Pruning must not touch a host the fleet still manages."""
    escalation = controller.STRANDED_ESCALATION_PASSES
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-prune-") as raw:
        state_dir = Path(raw)
        for index in range(escalation):
            status, _calls = _stranded_pass(
                controller, state_dir, FIRST_PASS + index * PASS_INTERVAL
            )
        stranding = _stranding(controller, state_dir)
    if status != controller.STRANDED_STATUS:
        failures.append(
            f"a producer held at zero across {escalation} passes exited {status} instead of "
            f"{controller.STRANDED_STATUS}; the cleanup swallowed a real stranding"
        )
    entry = stranding.get("producer")
    if not isinstance(entry, dict) or entry.get("passes") != escalation:
        failures.append(f"the drained producer's record is {entry} after {escalation} passes")


def _uninspected_host_keeps_its_record(controller: ModuleType, failures: list[str]) -> None:
    """A managed host this pass never looked at keeps what it earned.

    A consumer blocked behind a failed producer is never inspected, so nothing
    proves it climbed off zero.  Only leaving the declaration may drop a record.
    """
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-prune-") as raw:
        state_dir = Path(raw)
        _seed(controller, state_dir, {"consumer": {"since": RETIRED_SINCE, "passes": 1}})
        _status, calls = _stranded_pass(controller, state_dir, FIRST_PASS)
        stranding = _stranding(controller, state_dir)
    if ("check", "consumer") in calls:
        failures.append("the consumer was inspected, so this case no longer pins a blocked host")
    if "consumer" not in stranding:
        failures.append(
            "a blocked consumer that was never inspected lost its stranded-at-zero record, so "
            "a host sitting at zero behind a failed producer stops being counted"
        )


def _check_mode_reports_the_record_as_it_stands(
    controller: ModuleType, failures: list[str]
) -> None:
    """A read-only pass may not announce a cleanup it will not persist."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-prune-") as raw:
        state_dir = Path(raw)
        _seed(controller, state_dir, _retired())
        before = _document(controller, state_dir)
        _healthy_pass(controller, state_dir, FIRST_PASS, mode="check")
        after = _document(controller, state_dir)
    if before != after:
        failures.append("a check pass wrote to the state file")


def _pruning_is_narrow(controller: ModuleType, failures: list[str]) -> None:
    """Only hosts outside the declaration may be dropped."""
    stranding = {
        "retired-runner": {"since": RETIRED_SINCE, "passes": RETIRED_PASSES},
        "producer": {"since": RETIRED_SINCE, "passes": 2},
        "also-gone": {"since": RETIRED_SINCE, "passes": 1},
    }
    pruned = controller.prune_unmanaged_stranding(stranding, ["producer", "consumer"], FIRST_PASS)
    if pruned != ["also-gone", "retired-runner"]:
        failures.append(f"pruning took {pruned} instead of every unmanaged host, in order")
    if set(stranding) != {"producer"}:
        failures.append("pruning dropped a record for a host the fleet still manages")
    if controller.prune_unmanaged_stranding(stranding, ["producer"], FIRST_PASS):
        failures.append("pruning reported work on a record that had nothing unmanaged left")
    if controller.prune_unmanaged_stranding({}, [], FIRST_PASS):
        failures.append("pruning reported work on an empty record")


def _a_renamed_producer_releases_nobody(controller: ModuleType, failures: list[str]) -> None:
    """A stale record may not stand in as proof that an image is frozen.

    ``consumers_released`` reads the record as evidence that the producer was
    drained and is therefore publishing nothing.  A producer replaced by a new
    host leaves a record naming a host the controller no longer touches, and
    trusting it would release every consumer onto whatever the new producer is
    doing right now.
    """
    stranding = {
        "old-producer": {"since": RETIRED_SINCE, "passes": controller.PRODUCER_BLOCK_PASSES}
    }
    controller.prune_unmanaged_stranding(stranding, ["new-producer", "consumer"], FIRST_PASS)
    if controller.consumers_released(stranding, "old-producer", undrained=False):
        failures.append(
            "a replaced producer's stale record still released the consumers, so they would "
            "converge against an image the live producer may be republishing"
        )


def run(controller: ModuleType) -> list[str]:
    """Return every retired-host stranding-record failure."""
    failures: list[str] = []
    _retired_host_stops_failing_a_healthy_fleet(controller, failures)
    _second_pass_stays_quiet(controller, failures)
    _managed_host_still_escalates(controller, failures)
    _uninspected_host_keeps_its_record(controller, failures)
    _check_mode_reports_the_record_as_it_stands(controller, failures)
    _pruning_is_narrow(controller, failures)
    _a_renamed_producer_releases_nobody(controller, failures)
    return failures
