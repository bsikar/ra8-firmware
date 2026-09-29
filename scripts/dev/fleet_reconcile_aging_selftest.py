# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Regression selftests for aging a host's stranded-at-zero record (#888).

The stranded-at-zero record is the only thing that can tell one transient
failure from a fleet that has been sitting at zero capacity for days, and
``stranded_escalations`` turns a long enough record into a CRITICAL and
``STRANDED_STATUS``.  Only a pass that actually LOST capacity counted towards
it, though, which is the right guard on the claim that this pass drained a
host but far too narrow for the record's real claim: that the fleet is not
recovering.  A pass that reached a host already at zero and left it there
without mutating anything counted nothing at all, so the counter froze
wherever the last capacity-losing pass had left it.

Two everyday shapes froze it below the escalation threshold for good:

* a read-only check that keeps failing against a host that is already
  drained, which mutates nothing and deliberately claims no capacity loss;
* a consumer skipped behind a failed producer, which is never inspected.

Either way the host holds zero capacity, pass after pass, while the exit
status reads like any one-off failure and nothing ever escalates.  That is the
silence issue #888 went unnoticed in five times, so these tests pin the record
aging on every pass that leaves a drained host at zero, and pin that aging to
claiming nothing else: no receipt, no drain budget spent, and nothing at all
written by a check pass.
"""

from __future__ import annotations

import json
import tempfile
from collections.abc import Sequence
from pathlib import Path
from types import ModuleType
from typing import Any

import fleet_reconcile_process as frp

CHECK_FAILURE_STATUS = 2
APPLY_FAILURE_STATUS = 1
FIRST_PASS = 5000
SECOND_PASS = 6000
DRAINED_AT = 100
SEEDED_PASSES = 1
FULL_INTERVAL = 1000
PRODUCER_INTERVAL = 500
DIGEST = "d" * 64
CONSUMERS = ("consumer-a", "consumer-b")

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


def _data(consumers: Sequence[str] = CONSUMERS) -> dict[str, Any]:
    """Return one image producer ahead of the capacity-managed consumers."""
    hosts: dict[str, Any] = {
        "producer": {
            "class": "docker_linux",
            "runners": {"instances": 1},
            "provisions": ["one", "two"],
        }
    }
    for name in consumers:
        hosts[name] = {"class": "docker_linux", "runners": {"instances": 1}, "provisions": ["one"]}
    return {"runner_image": {"source_host": "producer"}, "hosts": hosts}


def _check(
    controller: ModuleType, data: dict[str, Any], host: str, changed: int
) -> frp.CommandResult:
    """Return one successful check whose first play reports ``changed``."""
    name = controller.recap_identity(data, host)
    row = f"{name} : ok=9 changed={{}} unreachable=0 failed=0 skipped=1 rescued=0 ignored=0\n"
    plays = len(data["hosts"][host]["provisions"])
    return frp.CommandResult(0, row.format(changed) + row.format(0) * (plays - 1), "")


def _noise(controller: ModuleType, data: dict[str, Any], host: str) -> int:
    """Return the check noise one converged host of this kind reports."""
    producer = host == data["runner_image"]["source_host"]
    return controller.PRODUCER_CHECK_NOISE if producer else 0


def _options(controller: ModuleType, state_dir: Path, now: int, mode: str = "apply") -> object:
    """Return deterministic policy for one pass.

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


def _receipt(now: int = FIRST_PASS) -> dict[str, Any]:
    """Return an ordinary receipt earned by a converged host."""
    return {"checked_at": now - 1, "full_applied_at": now - 1, "source_digest": DIGEST}


def _stranded(hosts: Sequence[str], passes: int = SEEDED_PASSES) -> dict[str, dict[str, int]]:
    """Return a stranded-at-zero record left behind by earlier passes."""
    return {host: {"since": DRAINED_AT, "passes": passes} for host in hosts}


def _seed(controller: ModuleType, state_dir: Path, document: dict[str, Any]) -> None:
    """Write one starting state file."""
    (state_dir / controller.STATE_FILE).write_text(json.dumps(document), encoding="ascii")


def _document(controller: ModuleType, state_dir: Path) -> dict[str, Any]:
    """Return the state file as it stands on disk."""
    path = state_dir / controller.STATE_FILE
    return json.loads(path.read_text(encoding="ascii")) if path.exists() else {}


def _failed_check_pass(  # noqa: PLR0913  # one pass's inputs plus the fleet shape it runs over
    controller: ModuleType,
    state_dir: Path,
    now: int,
    unreachable: set[str],
    *,
    mode: str = "apply",
    consumers: Sequence[str] = ("consumer-a",),
) -> tuple[int, list[tuple[str, str]], dict[str, Any]]:
    """Run one pass whose read-only check fails against the named hosts."""
    data = _data(consumers)
    issued: list[tuple[str, str]] = []

    def run(argv: Sequence[str]) -> frp.CommandResult:
        verb, host = _identity(argv)
        issued.append((verb, host))
        if verb == "check" and host in unreachable:
            return frp.CommandResult(CHECK_FAILURE_STATUS, "", f"{host}: host unreachable\n")
        if verb in {"check", "parked-check"}:
            return _check(controller, data, host, _noise(controller, data, host))
        return frp.CommandResult(0, "", "")

    status = controller.reconcile(
        data, _options(controller, state_dir, now, mode), run, sleep=_no_wait
    )
    return status, issued, _document(controller, state_dir)


def _blocked_consumer_pass(
    controller: ModuleType, state_dir: Path, now: int
) -> tuple[int, list[tuple[str, str]], dict[str, Any]]:
    """Run one pass whose drifting producer cannot apply and is drained."""
    data = _data(("consumer-a",))
    issued: list[tuple[str, str]] = []

    def run(argv: Sequence[str]) -> frp.CommandResult:
        verb, host = _identity(argv)
        issued.append((verb, host))
        if verb in {"check", "parked-check"}:
            drift = 1 if host == "producer" else 0
            return _check(controller, data, host, _noise(controller, data, host) + drift)
        if verb == "parked-apply" and host == "producer":
            return frp.CommandResult(APPLY_FAILURE_STATUS, "", "runner image build failed\n")
        return frp.CommandResult(0, "", "")

    status = controller.reconcile(data, _options(controller, state_dir, now), run, sleep=_no_wait)
    return status, issued, _document(controller, state_dir)


def _entry(document: dict[str, Any], host: str) -> dict[str, Any]:
    """Return one host's stranded-at-zero record from the state file."""
    stored = document.get("stranded")
    entry = stored.get(host) if isinstance(stored, dict) else None
    return entry if isinstance(entry, dict) else {}


def _touched(issued: Sequence[tuple[str, str]], host: str) -> list[str]:
    """Return every verb this pass issued against one host."""
    return [verb for verb, target in issued if target == host]


def _case_failing_check_still_ages(controller: ModuleType, failures: list[str]) -> None:
    """A drained host whose check keeps failing must reach escalation."""
    escalation = controller.STRANDED_ESCALATION_PASSES
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-aging-") as raw:
        state_dir = Path(raw)
        _seed(
            controller,
            state_dir,
            {"version": 1, "hosts": {}, "stranded": _stranded(("consumer-a",))},
        )
        statuses: list[int] = []
        document: dict[str, Any] = {}
        for index in range(escalation):
            status, issued, document = _failed_check_pass(
                controller, state_dir, FIRST_PASS + index * 1000, {"consumer-a"}
            )
            statuses.append(status)
            if _touched(issued, "consumer-a") != ["check"]:
                failures.append(
                    "aging: a failed read-only check issued "
                    f"{_touched(issued, 'consumer-a')} against a host already at zero"
                )
    entry = _entry(document, "consumer-a")
    if entry.get("passes") != SEEDED_PASSES + escalation:
        failures.append(
            f"aging: a host held at zero across {escalation} further passes has record {entry}; "
            "a read-only failure that mutates nothing froze the counter, so the host sits at "
            "zero capacity for good and never escalates"
        )
    if entry.get("since") != DRAINED_AT:
        failures.append(f"aging: the record's first-drained time moved to {entry.get('since')}")
    if controller.STRANDED_STATUS not in statuses:
        failures.append(
            f"aging: passes over a host stranded at zero exited {statuses}, never "
            f"{controller.STRANDED_STATUS}; the timer stays quiet while capacity is gone"
        )


def _case_blocked_consumer_ages(controller: ModuleType, failures: list[str]) -> None:
    """A consumer skipped behind a failed producer is still at zero."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-aging-") as raw:
        state_dir = Path(raw)
        _seed(
            controller,
            state_dir,
            {"version": 1, "hosts": {}, "stranded": _stranded(("consumer-a",))},
        )
        _status, issued, document = _blocked_consumer_pass(controller, state_dir, FIRST_PASS)
    if ("check", "consumer-a") in issued:
        failures.append("aging: the consumer was inspected, so this case no longer pins a hold")
    entry = _entry(document, "consumer-a")
    if entry.get("passes") != SEEDED_PASSES + 1:
        failures.append(
            f"aging: a consumer held at zero behind a failed producer has record {entry}; "
            "the pass it spent at zero was never counted"
        )


def _case_aging_claims_nothing_else(controller: ModuleType, failures: list[str]) -> None:
    """Aging a record may not publish a receipt or spend the drain budget."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-aging-") as raw:
        state_dir = Path(raw)
        _seed(
            controller,
            state_dir,
            {
                "version": 1,
                "hosts": {"consumer-b": _receipt()},
                "stranded": _stranded(("consumer-a",)),
            },
        )
        status, issued, document = _failed_check_pass(
            controller, state_dir, FIRST_PASS, {"consumer-a"}, consumers=CONSUMERS
        )
    if "consumer-a" in document.get("hosts", {}):
        failures.append("aging: a host still at zero published a receipt")
    if _entry(document, "consumer-b"):
        failures.append("aging: a serving consumer was recorded at zero by another host's failure")
    if "quarantine" in _touched(issued, "consumer-b"):
        failures.append(
            "aging: the second consumer was drained; aging one record must not count as this "
            "pass losing capacity"
        )
    if "check" not in _touched(issued, "consumer-b"):
        failures.append(
            "aging: the second consumer was never inspected, so aging a record spent the "
            "pass's drain budget"
        )
    if status != 1:
        failures.append(
            f"aging: one pass over a host below the escalation threshold exited {status}, "
            "not an ordinary failure"
        )


def _case_check_mode_counts_nothing(controller: ModuleType, failures: list[str]) -> None:
    """A check pass persists nothing, so it may not count a pass at zero."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-aging-") as raw:
        state_dir = Path(raw)
        seeded = {"version": 1, "hosts": {}, "stranded": _stranded(("consumer-a",))}
        _seed(controller, state_dir, seeded)
        _status, _issued, document = _failed_check_pass(
            controller, state_dir, FIRST_PASS, {"consumer-a"}, mode="check"
        )
    if _entry(document, "consumer-a") != {"since": DRAINED_AT, "passes": SEEDED_PASSES}:
        failures.append(
            f"aging: a check pass rewrote the record to {_entry(document, 'consumer-a')}"
        )


def _case_serving_host_earns_no_record(controller: ModuleType, failures: list[str]) -> None:
    """A failed check against a SERVING host still records no stranding."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-aging-") as raw:
        state_dir = Path(raw)
        _seed(controller, state_dir, {"version": 1, "hosts": {"consumer-a": _receipt()}})
        _status, issued, document = _failed_check_pass(
            controller, state_dir, FIRST_PASS, {"consumer-a"}
        )
    if document.get("stranded"):
        failures.append(
            f"aging: a read-only failure recorded {document.get('stranded')} at zero; the host "
            "kept serving and nothing was mutated"
        )
    if "quarantine" in _touched(issued, "consumer-a"):
        failures.append("aging: a read-only check failure drained a serving host")


def _case_aging_arithmetic(controller: ModuleType, failures: list[str]) -> None:
    """Pin what age_stranding does, and what it refuses to invent."""
    stranding = _stranded(("consumer-a",), passes=SEEDED_PASSES + 1)
    if not controller.age_stranding(stranding, "consumer-a", SECOND_PASS, "unit"):
        failures.append("aging: age_stranding did not report counting a pass at zero")
    if stranding["consumer-a"] != {"since": DRAINED_AT, "passes": SEEDED_PASSES + 2}:
        failures.append(f"aging: age_stranding left {stranding['consumer-a']}")
    if controller.age_stranding(stranding, "never-drained", SECOND_PASS, "unit"):
        failures.append("aging: age_stranding invented a record for a host with none")
    if "never-drained" in stranding:
        failures.append("aging: age_stranding recorded a host that was never drained")


def run(controller: ModuleType) -> list[str]:
    """Run every stranded-record aging case against the controller under test."""
    failures: list[str] = []
    _case_failing_check_still_ages(controller, failures)
    _case_blocked_consumer_ages(controller, failures)
    _case_aging_claims_nothing_else(controller, failures)
    _case_check_mode_counts_nothing(controller, failures)
    _case_serving_host_earns_no_record(controller, failures)
    _case_aging_arithmetic(controller, failures)
    return failures
