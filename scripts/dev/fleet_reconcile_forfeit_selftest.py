# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Regression selftests for a drain budget that counts parked hosts as serving (#888).

The pass drain budget keeps one reconcile pass from emptying the fleet, and it
is measured against the capacity that actually exists rather than against the
declaration: a host already recorded at ZERO has nothing left for the budget to
protect, so it is neither counted nor charged.

A DURABLE maintenance park is the other way a host holds no capacity.  The
marker is written before the drain that was then refused, the host-local window
timer refuses to raise admission while it is there, and only a capacity restore
removes it.  ``consumer_held`` already exempts such a host from the budget for
exactly the reason a stranded one is exempt, but ``serving_hosts`` still
counted it among the hosts STILL SERVING, so the asymmetry the budget was
tightened to remove came straight back through the newer record: a fleet whose
hosts were parked one by one by refused drains kept handing every later pass a
budget derived from hosts whose admission nothing but this controller can
raise, and the pass was free to drain everything still serving behind them.
That is issue #888 arriving in instalments again, and the pass reports it as an
ordinary failure instead of halting.

These tests pin the budget to the capacity that is genuinely in service:

* a pass over a fleet with parked hosts may only drain a share of what is
  really serving, and halts with ``CASCADE_STATUS`` rather than emptying it;
* the exemption survives, so a parked host is still reached and repaired in
  that same pass;
* the floor of one survives, so a broken host is never left serving for want
  of budget;
* a fleet with nothing parked and nothing at zero is budgeted exactly as
  before;
* the arithmetic itself, over both records at once.
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
FULL_INTERVAL = 100000
PRODUCER_INTERVAL = 100000
DIGEST = "d" * 64
CONSUMERS = ("consumer-a", "consumer-b", "consumer-c", "consumer-d")

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


def _runner(  # noqa: PLR0913  # the fleet plus every fault one pass has to carry
    controller: ModuleType,
    data: dict[str, Any],
    issued: list[tuple[str, str]],
    failing: set[str],
    unreadable: set[str] = frozenset(),
    refuse_restore: set[str] = frozenset(),
) -> Any:  # noqa: ANN401  # the controller decides what a command runner is
    """Return a runner where every failing host has drifted and cannot apply.

    ``unreadable`` hosts fail their read-only check instead, which mutates
    nothing: that is the everyday shape of a host already down, and the one
    that leaves a park in place for the whole pass because nothing applies it
    and so no receipt is published for it.
    """

    def run(argv: Sequence[str]) -> frp.CommandResult:
        verb, host = _identity(argv)
        issued.append((verb, host))
        if verb == "restore" and host in refuse_restore:
            return frp.CommandResult(9, "", f"{host}: restore refused\n")
        if verb in {"check", "parked-check"}:
            if verb == "check" and host in unreadable:
                return frp.CommandResult(2, "", f"{host}: unreachable\n")
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
    return {"checked_at": NOW - 1000, "full_applied_at": NOW - 1000, "source_digest": DIGEST}


def _parked(hosts: Sequence[str], passes: int = 1) -> dict[str, dict[str, int]]:
    """Return the durable-park record refused drains left behind."""
    return {host: {"since": NOW - 4000, "passes": passes} for host in hosts}


def _seed(controller: ModuleType, state_dir: Path, document: dict[str, Any]) -> None:
    """Write one starting state file."""
    (state_dir / controller.STATE_FILE).write_text(json.dumps(document), encoding="ascii")


def _document(controller: ModuleType, state_dir: Path) -> dict[str, Any]:
    """Return the state file as it stands on disk."""
    path = state_dir / controller.STATE_FILE
    return json.loads(path.read_text(encoding="ascii")) if path.exists() else {}


def _run_pass(  # noqa: PLR0913  # one pass over a seeded fleet with its faults
    controller: ModuleType,
    state_dir: Path,
    failing: set[str],
    *,
    consumers: Sequence[str] = CONSUMERS,
    unreadable: set[str] = frozenset(),
    refuse_restore: set[str] = frozenset(),
) -> tuple[int, list[tuple[str, str]], dict[str, Any]]:
    """Run one pass and return its status, the verbs it issued and the state."""
    data = _data(consumers)
    issued: list[tuple[str, str]] = []
    status = controller.reconcile(
        data,
        _options(controller, state_dir),
        _runner(controller, data, issued, failing, unreadable, refuse_restore),
        sleep=_no_wait,
    )
    return status, issued, _document(controller, state_dir)


def _touched(issued: Sequence[tuple[str, str]], host: str) -> list[str]:
    """Return every verb this pass issued against one host."""
    return [verb for verb, target in issued if target == host]


def _drained(issued: Sequence[tuple[str, str]]) -> list[str]:
    """Return every host this pass took out of service."""
    return [host for verb, host in issued if verb == "quarantine"]


def _case_parked_capacity_is_not_serving(controller: ModuleType, failures: list[str]) -> None:
    """A fleet held parked may not hand the next pass a budget sized for a full one."""
    parked = ("consumer-a", "consumer-b")
    broken = {"consumer-c", "consumer-d"}
    with tempfile.TemporaryDirectory() as raw:
        state_dir = Path(raw)
        _seed(
            controller,
            state_dir,
            {
                "version": 1,
                "hosts": {host: _receipt() for host in ("producer", *CONSUMERS)},
                "stranded": {},
                "parked": _parked(parked),
            },
        )
        status, issued, document = _run_pass(controller, state_dir, broken, unreadable=set(parked))
    if _drained(issued) != ["consumer-c"]:
        failures.append(
            f"forfeit: pass took {_drained(issued)} to zero, expected the budget of one; "
            "the parked hosts were counted as capacity still serving"
        )
    if _touched(issued, "consumer-d"):
        failures.append(
            f"forfeit: held consumer-d was still touched with {_touched(issued, 'consumer-d')}"
        )
    if document["hosts"].get("consumer-d") != _receipt():
        failures.append("forfeit: a host the pass never touched lost its receipt")
    if status != controller.CASCADE_STATUS:
        failures.append(
            f"forfeit: pass exited {status}, expected CASCADE_STATUS; a pass that empties "
            "the capacity behind parked hosts must not read like an ordinary failure"
        )


def _case_parked_host_is_still_repaired(controller: ModuleType, failures: list[str]) -> None:
    """The exemption survives: a parked host is reached in the same tightened pass."""
    parked = ("consumer-a", "consumer-b")
    with tempfile.TemporaryDirectory() as raw:
        state_dir = Path(raw)
        _seed(
            controller,
            state_dir,
            {
                "version": 1,
                "hosts": {},
                "stranded": {},
                "parked": _parked(parked),
            },
        )
        status, issued, document = _run_pass(controller, state_dir, {"consumer-c", "consumer-d"})
    for host in parked:
        if "parked-apply" not in _touched(issued, host):
            failures.append(f"forfeit: the tighter budget held back the repair of {host}")
        if "restore" not in _touched(issued, host):
            failures.append(f"forfeit: parked {host} was never reopened by this pass")
        if host in document.get("parked", {}):
            failures.append(f"forfeit: repaired {host} kept its durable-park record")
    if status not in {APPLY_FAILURE_STATUS, controller.CASCADE_STATUS}:
        failures.append(f"forfeit: pass exited {status}, expected a failure verdict")


def _case_last_serving_host_is_drainable(controller: ModuleType, failures: list[str]) -> None:
    """The floor of one survives: a broken host is never left serving for want of budget."""
    consumers = ("consumer-a", "consumer-b")
    with tempfile.TemporaryDirectory() as raw:
        state_dir = Path(raw)
        _seed(
            controller,
            state_dir,
            {
                "version": 1,
                "hosts": {},
                "stranded": {},
                "parked": _parked(("consumer-b",)),
            },
        )
        _status, issued, _document = _run_pass(
            controller,
            state_dir,
            {"consumer-a"},
            consumers=consumers,
            unreadable={"consumer-b"},
        )
    if "consumer-a" not in _drained(issued):
        failures.append("forfeit: the last serving consumer was left serving a failed mutation")


def _case_healthy_fleet_is_unchanged(controller: ModuleType, failures: list[str]) -> None:
    """With nothing parked and nothing at zero the budget is what it always was."""
    with tempfile.TemporaryDirectory() as raw:
        state_dir = Path(raw)
        status, issued, document = _run_pass(controller, state_dir, {"consumer-a"})
    if _drained(issued) != ["consumer-a"]:
        failures.append(
            f"forfeit: a healthy fleet drained {_drained(issued)}, expected the one broken host"
        )
    if status != APPLY_FAILURE_STATUS:
        failures.append(f"forfeit: a lone failure exited {status}, expected an ordinary failure")
    failures.extend(
        f"forfeit: {host} was held back on a fleet with nothing forfeit"
        for host in ("consumer-b", "consumer-c", "consumer-d")
        if host not in document["hosts"]
    )


def _case_forfeit_arithmetic(controller: ModuleType, failures: list[str]) -> None:
    """Pin what counts as serving, and the budget, over both records at once."""
    order = ["producer", "consumer-a", "consumer-b", "consumer-c"]
    stranding = {"consumer-b": {"since": NOW - 900, "passes": 1}}
    parked = _parked(("consumer-c",))
    serving = controller.serving_hosts(order, stranding, parked)
    if serving != ["producer", "consumer-a"]:
        failures.append(f"forfeit: serving_hosts returned {serving}")
    if controller.serving_hosts(order, {}, {}) != order:
        failures.append("forfeit: serving_hosts dropped a host on a fleet with nothing forfeit")
    if controller.serving_hosts(order, stranding) != ["producer", "consumer-a", "consumer-c"]:
        failures.append("forfeit: serving_hosts ignored its default of no parked record")
    budgets = {
        "both records": controller.open_drain_budget(order, stranding, parked),
        "parked only": controller.open_drain_budget(order, {}, _parked(("consumer-b",))),
        "nothing forfeit": controller.open_drain_budget(order, {}, {}),
        "all parked": controller.open_drain_budget(order, {}, _parked(order)),
    }
    expected = {"both records": 1, "parked only": 1, "nothing forfeit": 2, "all parked": 1}
    if budgets != expected:
        failures.append(f"forfeit: open_drain_budget returned {budgets}, expected {expected}")


def _case_refused_release_keeps_capacity_forfeit(
    controller: ModuleType, failures: list[str]
) -> None:
    """A park this pass could not lift stays forfeit for the next pass's budget."""
    with tempfile.TemporaryDirectory() as raw:
        state_dir = Path(raw)
        _seed(
            controller,
            state_dir,
            {
                "version": 1,
                "hosts": {},
                "stranded": {},
                "parked": _parked(("consumer-a",), passes=2),
            },
        )
        status, issued, document = _run_pass(
            controller, state_dir, set(), refuse_restore={"consumer-a"}
        )
    if "restore" not in _touched(issued, "consumer-a"):
        failures.append("forfeit: no restore was issued for the parked host")
    if "consumer-a" not in document.get("parked", {}):
        failures.append("forfeit: a park whose restore was REFUSED lost its record")
    if status != controller.STRANDED_STATUS:
        failures.append(
            f"forfeit: a refused park release exited {status}, expected STRANDED_STATUS"
        )
    order = controller.runner_hosts(_data())
    if "consumer-a" in controller.serving_hosts(order, {}, document["parked"]):
        failures.append("forfeit: the still-parked host counts as serving for the next pass")


def run(controller: ModuleType) -> list[str]:
    """Run every forfeit-capacity budget case against the controller under test."""
    failures: list[str] = []
    _case_parked_capacity_is_not_serving(controller, failures)
    _case_parked_host_is_still_repaired(controller, failures)
    _case_last_serving_host_is_drainable(controller, failures)
    _case_healthy_fleet_is_unchanged(controller, failures)
    _case_forfeit_arithmetic(controller, failures)
    _case_refused_release_keeps_capacity_forfeit(controller, failures)
    return failures
