# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Regression selftests for one pass emptying the whole fleet (#888).

Draining a host after a failed mutation is the right fail-closed reflex for
ONE host: whatever the mutation left behind, the host stops taking work.
Repeated across a pass it stops being convergence and becomes an evacuation.
A provision that is broken in the snapshot fails on every host that carries it,
so the controller drained consumer after consumer inside a single pass, took
the fleet to zero capacity, exited 1 like any ordinary failure, and then did
exactly the same thing on the next pass.  No amount of further draining can fix
a fault that lives in the declaration, and issue #888 counted about five
strandings at zero that nobody caught because a fleet with no capacity left
read exactly like a one-off failure.

These tests pin the narrow guard:

* a pass stops mutating once it has taken its budget of SERVING hosts to zero,
  leaves the remaining capacity alone and untouched, and says so with a verdict
  of its own instead of a bare failure;
* a host already recorded at zero is never held back, because repairing it is
  the recovery this controller exists for and it has no capacity left to lose;
* the consumer release onto a drained producer's frozen image still works, on
  the tightest budget there is.
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
BLOCK_PASSES = 3
CONSUMERS = ("consumer-a", "consumer-b", "consumer-c")

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


def _clean(controller: ModuleType, data: dict[str, Any], host: str) -> frp.CommandResult:
    """Return the exact accepted check result for one converged host."""
    return _check(controller, data, host, _noise(controller, data, host))


def _runner(
    controller: ModuleType, data: dict[str, Any], issued: list[tuple[str, str]], failing: set[str]
) -> Any:  # noqa: ANN401  # the controller decides what a command runner is
    """Return a runner where every failing host has drifted and cannot apply.

    Drift is what a broken provision looks like from a check, and it is also
    what keeps the last-known-good recovery out of these cases: a host that was
    never converged has nothing to reopen.
    """

    def run(argv: Sequence[str]) -> frp.CommandResult:
        verb, host = _identity(argv)
        issued.append((verb, host))
        if verb in {"check", "parked-check"}:
            if verb == "check" and host in failing:
                return _check(controller, data, host, _noise(controller, data, host) + 1)
            return _clean(controller, data, host)
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


def _seed(controller: ModuleType, state_dir: Path, document: dict[str, Any]) -> None:
    """Write one starting state file."""
    (state_dir / controller.STATE_FILE).write_text(json.dumps(document), encoding="ascii")


def _document(controller: ModuleType, state_dir: Path) -> dict[str, Any]:
    """Return the state file as it stands on disk."""
    path = state_dir / controller.STATE_FILE
    return json.loads(path.read_text(encoding="ascii")) if path.exists() else {}


def _receipt() -> dict[str, Any]:
    """Return an ordinary receipt earned by a converged host."""
    return {"checked_at": NOW - 1, "full_applied_at": NOW - 1, "source_digest": DIGEST}


def _run_pass(
    controller: ModuleType,
    state_dir: Path,
    failing: set[str],
    *,
    consumers: Sequence[str] = CONSUMERS,
    mode: str = "apply",
) -> tuple[int, list[tuple[str, str]], dict[str, Any]]:
    """Run one pass and return its status, the verbs it issued and the state."""
    data = _data(consumers)
    issued: list[tuple[str, str]] = []
    status = controller.reconcile(
        data,
        _options(controller, state_dir, mode),
        _runner(controller, data, issued, failing),
        sleep=_no_wait,
    )
    return status, issued, _document(controller, state_dir)


def _touched(issued: Sequence[tuple[str, str]], host: str) -> list[str]:
    """Return every verb this pass issued against one host."""
    return [verb for verb, target in issued if target == host]


def _case_budget_halts_the_pass(controller: ModuleType, failures: list[str]) -> None:
    """A fault reproducing on every consumer stops after the budget, loudly."""
    with tempfile.TemporaryDirectory() as raw:
        state_dir = Path(raw)
        _seed(controller, state_dir, {"version": 1, "hosts": {"consumer-c": _receipt()}})
        status, issued, document = _run_pass(controller, state_dir, set(CONSUMERS))
    drained = [host for verb, host in issued if verb == "quarantine"]
    if drained != ["consumer-a", "consumer-b"]:
        failures.append(f"cascade: pass drained {drained}, expected the budget of two")
    if _touched(issued, "consumer-c"):
        failures.append(
            f"cascade: held consumer-c was still touched with {_touched(issued, 'consumer-c')}"
        )
    if status != controller.CASCADE_STATUS:
        failures.append(f"cascade: halted pass exited {status}, expected CASCADE_STATUS")
    if document["hosts"].get("consumer-c") != _receipt():
        failures.append("cascade: a host the pass never touched lost its receipt")
    if sorted(document["stranded"]) != ["consumer-a", "consumer-b"]:
        failures.append(f"cascade: stranded record is {sorted(document['stranded'])}")


def _case_one_failure_still_drains(controller: ModuleType, failures: list[str]) -> None:
    """The guard is narrow: a single failure drains and reports as it always did."""
    with tempfile.TemporaryDirectory() as raw:
        state_dir = Path(raw)
        status, issued, document = _run_pass(controller, state_dir, {"consumer-a"})
    if [host for verb, host in issued if verb == "quarantine"] != ["consumer-a"]:
        failures.append("cascade: a lone failure did not drain exactly its own host")
    if status != APPLY_FAILURE_STATUS:
        failures.append(f"cascade: a lone failure exited {status}, expected an ordinary failure")
    for host in ("consumer-b", "consumer-c"):
        if "parked-apply" not in _touched(issued, host):
            failures.append(f"cascade: {host} was held back by a single unrelated failure")
        if host not in document["hosts"]:
            failures.append(f"cascade: {host} reconciled without publishing a receipt")


def _case_recovery_is_never_held(controller: ModuleType, failures: list[str]) -> None:
    """A host already at zero is repaired even once the budget is spent."""
    with tempfile.TemporaryDirectory() as raw:
        state_dir = Path(raw)
        _seed(
            controller,
            state_dir,
            {
                "version": 1,
                "hosts": {},
                "stranded": {"consumer-c": {"since": NOW - 900, "passes": 2}},
            },
        )
        status, issued, document = _run_pass(controller, state_dir, {"consumer-a", "consumer-b"})
    if "parked-apply" not in _touched(issued, "consumer-c"):
        failures.append("cascade: the budget held back the repair of a host already at zero")
    if "consumer-c" not in document["hosts"]:
        failures.append("cascade: the repaired host published no receipt")
    if document["stranded"].get("consumer-c") is not None:
        failures.append("cascade: the repaired host kept its stranded-at-zero record")
    if status != APPLY_FAILURE_STATUS:
        failures.append(f"cascade: pass exited {status}; nothing serving was held back")


def _case_release_survives_the_budget(controller: ModuleType, failures: list[str]) -> None:
    """Draining a producer that is already at zero does not spend the budget."""
    consumers = ("consumer-a", "consumer-b")
    with tempfile.TemporaryDirectory() as raw:
        state_dir = Path(raw)
        _seed(
            controller,
            state_dir,
            {
                "version": 1,
                "hosts": {},
                "stranded": {"producer": {"since": NOW - 900, "passes": BLOCK_PASSES}},
            },
        )
        _status, issued, document = _run_pass(
            controller, state_dir, {"producer"}, consumers=consumers
        )
    for host in consumers:
        if "parked-apply" not in _touched(issued, host):
            failures.append(f"cascade: released {host} was held by the drain budget")
        receipt = document["hosts"].get(host, {})
        if receipt.get(controller.RELEASED_RECEIPT_KEY) != "producer":
            failures.append(f"cascade: released {host} earned {receipt}, not a provisional one")


def _case_check_mode_holds_nothing(controller: ModuleType, failures: list[str]) -> None:
    """A check pass drains nothing, so it can never spend a drain budget."""
    with tempfile.TemporaryDirectory() as raw:
        state_dir = Path(raw)
        status, issued, _document = _run_pass(controller, state_dir, set(CONSUMERS), mode="check")
    if status != APPLY_FAILURE_STATUS:
        failures.append(f"cascade: drifting check pass exited {status}, expected a plain failure")
    if [verb for verb, _host in issued if verb != "check"]:
        failures.append("cascade: a check pass issued a mutating verb")
    failures.extend(
        f"cascade: check mode skipped {host}"
        for host in CONSUMERS
        if _touched(issued, host) != ["check"]
    )


def _case_budget_and_hold_policy(controller: ModuleType, failures: list[str]) -> None:
    """Pin the budget arithmetic and every hold the pass can apply."""
    budgets = {size: controller.drain_budget(size) for size in (1, 2, 3, 4, 5, 10)}
    if budgets != {1: 1, 2: 1, 3: 1, 4: 2, 5: 2, 10: 5}:
        failures.append(f"cascade: drain_budget returned {budgets}")
    stranded = {"at-zero": {"since": NOW, "passes": 1}}
    halted: list[str] = []
    holds = {
        "blocking": controller.consumer_held(
            "serving", {}, halted, blocking=True, drained=["a", "b"], budget=1
        ),
        "under-budget": controller.consumer_held(
            "serving", {}, halted, blocking=False, drained=["a"], budget=2
        ),
        "at-budget": controller.consumer_held(
            "serving", {}, halted, blocking=False, drained=["a", "b"], budget=2
        ),
        "already-at-zero": controller.consumer_held(
            "at-zero", stranded, halted, blocking=False, drained=["a", "b"], budget=2
        ),
    }
    expected = {
        "blocking": True,
        "under-budget": False,
        "at-budget": True,
        "already-at-zero": False,
    }
    if holds != expected:
        failures.append(f"cascade: consumer_held returned {holds}")
    if halted != ["serving"]:
        failures.append(f"cascade: the halted list collected {halted}, expected the budget hold")


def run(controller: ModuleType) -> list[str]:
    """Run every cascade-halt case against the controller under test."""
    failures: list[str] = []
    _case_budget_halts_the_pass(controller, failures)
    _case_one_failure_still_drains(controller, failures)
    _case_recovery_is_never_held(controller, failures)
    _case_release_survives_the_budget(controller, failures)
    _case_check_mode_holds_nothing(controller, failures)
    _case_budget_and_hold_policy(controller, failures)
    return failures
