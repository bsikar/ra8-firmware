# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Regression selftests for a drain budget measured against the declaration (#888).

The pass drain budget exists to keep one reconcile pass from emptying the
fleet, and a host already recorded at zero is deliberately exempt from it: it
has no capacity left to lose and repairing it is the recovery this controller
exists for.  The budget itself, though, was derived from the size of the
DECLARATION, so every host an earlier pass left at zero quietly raised the
number of still-serving hosts the next pass was allowed to drain after it.  A
fleet already half down handed the next pass a budget big enough to take
everything that was left, one pass never looked like an evacuation, and the
fleet still ended at zero.  That is issue #888 arriving in instalments.

These tests pin the budget to the capacity that actually exists:

* a pass that starts with hosts already at zero may only drain a share of what
  is still SERVING, and leaves the rest untouched with their receipts;
* the exemption survives, so hosts already at zero are still repaired in that
  same pass;
* a healthy fleet is budgeted exactly as before, so nothing about an ordinary
  pass changes.
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
NOW = 5000
FULL_INTERVAL = 1000
PRODUCER_INTERVAL = 500
DIGEST = "d" * 64
CONSUMERS = ("consumer-a", "consumer-b", "consumer-c", "consumer-d", "consumer-e")

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


def _runner(
    controller: ModuleType, data: dict[str, Any], issued: list[tuple[str, str]], failing: set[str]
) -> Any:  # noqa: ANN401  # the controller decides what a command runner is
    """Return a runner where every failing host has drifted and cannot apply."""

    def run(argv: Sequence[str]) -> frp.CommandResult:
        verb, host = _identity(argv)
        issued.append((verb, host))
        if verb in {"check", "parked-check"}:
            extra = 1 if verb == "check" and host in failing else 0
            return _check(controller, data, host, _noise(controller, data, host) + extra)
        if verb == "parked-apply" and host in failing:
            return frp.CommandResult(2, "", f"{host}: provision failed\n")
        return frp.CommandResult(0, "", "")

    return run


def _options(controller: ModuleType, state_dir: Path, mode: str = "apply") -> object:
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
        now=NOW,
    )


def _no_wait(_seconds: float) -> None:
    """Take retry pacing out of these cases without skipping it."""


def _receipt() -> dict[str, Any]:
    """Return an ordinary receipt earned by a converged host."""
    return {"checked_at": NOW - 1, "full_applied_at": NOW - 1, "source_digest": DIGEST}


def _seed(controller: ModuleType, state_dir: Path, document: dict[str, Any]) -> None:
    """Write one starting state file."""
    (state_dir / controller.STATE_FILE).write_text(json.dumps(document), encoding="ascii")


def _document(controller: ModuleType, state_dir: Path) -> dict[str, Any]:
    """Return the state file as it stands on disk."""
    path = state_dir / controller.STATE_FILE
    return json.loads(path.read_text(encoding="ascii")) if path.exists() else {}


def _run_pass(
    controller: ModuleType,
    state_dir: Path,
    failing: set[str],
    *,
    consumers: Sequence[str] = CONSUMERS,
) -> tuple[int, list[tuple[str, str]], dict[str, Any]]:
    """Run one pass and return its status, the verbs it issued and the state."""
    data = _data(consumers)
    issued: list[tuple[str, str]] = []
    status = controller.reconcile(
        data,
        _options(controller, state_dir),
        _runner(controller, data, issued, failing),
        sleep=_no_wait,
    )
    return status, issued, _document(controller, state_dir)


def _touched(issued: Sequence[tuple[str, str]], host: str) -> list[str]:
    """Return every verb this pass issued against one host."""
    return [verb for verb, target in issued if target == host]


def _drained(issued: Sequence[tuple[str, str]]) -> list[str]:
    """Return every host this pass took out of service."""
    return [host for verb, host in issued if verb == "quarantine"]


def _stranded(hosts: Sequence[str], passes: int = 1) -> dict[str, dict[str, int]]:
    """Return a stranded-at-zero record left behind by earlier passes."""
    return {host: {"since": NOW - 900, "passes": passes} for host in hosts}


def _case_budget_follows_serving_capacity(controller: ModuleType, failures: list[str]) -> None:
    """A half-empty fleet may not spend a budget sized for a full one."""
    already_down = ("consumer-c", "consumer-d", "consumer-e")
    with tempfile.TemporaryDirectory() as raw:
        state_dir = Path(raw)
        _seed(
            controller,
            state_dir,
            {
                "version": 1,
                "hosts": {"consumer-a": _receipt(), "consumer-b": _receipt()},
                "stranded": _stranded(already_down),
            },
        )
        status, issued, document = _run_pass(controller, state_dir, set(CONSUMERS))
    serving_drained = [host for host in _drained(issued) if host not in already_down]
    if serving_drained != ["consumer-a"]:
        failures.append(
            f"budget: pass took {serving_drained} of the serving hosts to zero, "
            "expected the budget of one"
        )
    if _touched(issued, "consumer-b"):
        failures.append(
            f"budget: held consumer-b was still touched with {_touched(issued, 'consumer-b')}"
        )
    if document["hosts"].get("consumer-b") != _receipt():
        failures.append("budget: a host the pass never touched lost its receipt")
    if status != controller.CASCADE_STATUS:
        failures.append(f"budget: halted pass exited {status}, expected CASCADE_STATUS")


def _case_recovery_under_tighter_budget(controller: ModuleType, failures: list[str]) -> None:
    """Hosts already at zero are still repaired once the budget is spent."""
    already_down = ("consumer-c", "consumer-d", "consumer-e")
    with tempfile.TemporaryDirectory() as raw:
        state_dir = Path(raw)
        _seed(
            controller,
            state_dir,
            {"version": 1, "hosts": {}, "stranded": _stranded(already_down)},
        )
        status, issued, document = _run_pass(controller, state_dir, {"consumer-a", "consumer-b"})
    for host in already_down:
        if "parked-apply" not in _touched(issued, host):
            failures.append(f"budget: the tighter budget held back the repair of {host}")
        if host not in document["hosts"]:
            failures.append(f"budget: repaired {host} published no receipt")
        if host in document.get("stranded", {}):
            failures.append(f"budget: repaired {host} kept its stranded-at-zero record")
    if status != controller.CASCADE_STATUS:
        failures.append(f"budget: pass exited {status}; serving capacity was held back")


def _case_healthy_fleet_is_unchanged(controller: ModuleType, failures: list[str]) -> None:
    """With nothing recorded at zero the budget is exactly what it always was."""
    with tempfile.TemporaryDirectory() as raw:
        state_dir = Path(raw)
        status, issued, document = _run_pass(controller, state_dir, {"consumer-a"})
    if _drained(issued) != ["consumer-a"]:
        failures.append(f"budget: a healthy fleet drained {_drained(issued)}, expected one host")
    if status != APPLY_FAILURE_STATUS:
        failures.append(f"budget: a lone failure exited {status}, expected an ordinary failure")
    failures.extend(
        f"budget: {host} was held back on a fleet with nothing at zero"
        for host in ("consumer-b", "consumer-c", "consumer-d", "consumer-e")
        if host not in document["hosts"]
    )


def _case_last_serving_host_is_drainable(controller: ModuleType, failures: list[str]) -> None:
    """The floor of one survives: a broken host is never left serving for want of budget."""
    consumers = ("consumer-a", "consumer-b")
    with tempfile.TemporaryDirectory() as raw:
        state_dir = Path(raw)
        _seed(
            controller,
            state_dir,
            {"version": 1, "hosts": {}, "stranded": _stranded(("consumer-b",))},
        )
        _status, issued, _document = _run_pass(
            controller, state_dir, {"consumer-a"}, consumers=consumers
        )
    if "consumer-a" not in _drained(issued):
        failures.append("budget: the last serving consumer was left serving a failed mutation")


def _case_budget_arithmetic(controller: ModuleType, failures: list[str]) -> None:
    """Pin what counts as serving and the budget each fleet shape earns."""
    stranding = _stranded(("consumer-b", "consumer-c"))
    order = ["producer", "consumer-a", "consumer-b", "consumer-c"]
    serving = controller.serving_hosts(order, stranding)
    if serving != ["producer", "consumer-a"]:
        failures.append(f"budget: serving_hosts returned {serving}")
    if controller.serving_hosts(order, {}) != order:
        failures.append("budget: serving_hosts dropped a host on a fleet with nothing at zero")
    budgets = {
        "half down": controller.open_drain_budget(order, stranding),
        "none down": controller.open_drain_budget(order, {}),
        "all down": controller.open_drain_budget(order, _stranded(order)),
    }
    expected = {"half down": 1, "none down": 2, "all down": 1}
    if budgets != expected:
        failures.append(f"budget: open_drain_budget returned {budgets}, expected {expected}")


def run(controller: ModuleType) -> list[str]:
    """Run every serving-capacity budget case against the controller under test."""
    failures: list[str] = []
    _case_budget_follows_serving_capacity(controller, failures)
    _case_recovery_under_tighter_budget(controller, failures)
    _case_healthy_fleet_is_unchanged(controller, failures)
    _case_last_serving_host_is_drainable(controller, failures)
    _case_budget_arithmetic(controller, failures)
    return failures
