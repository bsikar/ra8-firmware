# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Regression selftests for records kept against hosts back in service (#888).

A failed pass can still end with capacity REOPENED and verified: an apply that
fails with no prior drift leaves the host running the declaration it was
already converged on, so the controller restores its last-known-good capacity
and proves it serves.  The pass fails, and every failure that cost no capacity
aged the stranded-at-zero record, so a host drained once and then restored on
every pass after it kept counting passes it spent SERVING.  Three of those and
the controller escalated CRITICAL and ``STRANDED_STATUS`` over a host carrying
work, which spends the loudness issue #888 exists to buy on a false alarm.  The
stale record is load-bearing in two more places, so it does more than shout:
the producer's consumers earn PROVISIONAL receipts against an image that is
being served, and the pass drain budget treats a serving host as having nothing
left to lose, so the next pass may take it to zero for free.

These tests pin the narrow clear:

* a host whose capacity a failed pass reopened and verified loses its record,
  stops escalating, and is counted among the hosts still serving again;
* a host genuinely still at zero keeps ageing and still escalates, so the
  counter a read-only failure or a blocked consumer advances is untouched;
* a refused drain whose reopen verified also clears, because that host is
  accounted for and serving;
* a check pass persists nothing, and no record is ever invented for a host
  that never had one.
"""

from __future__ import annotations

import tempfile
from collections.abc import Sequence
from pathlib import Path
from types import ModuleType
from typing import Any

import fleet_reconcile_process as frp

CHECK_FAILURE_STATUS = 2
APPLY_FAILURE_STATUS = 1
DRAIN_REFUSED_STATUS = 7
FIRST_PASS = 5000
PASS_INTERVAL = 10
FULL_INTERVAL = 1000
PRODUCER_INTERVAL = 500
DIGEST = "c" * 64
PRODUCER = "image-a"
CONSUMER = "consumer"

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
    """Return one image producer and one consumer that depends on its image."""
    return {
        "runner_image": {"source_host": PRODUCER},
        "hosts": {
            PRODUCER: {
                "class": "docker_linux",
                "runners": {"instances": 1},
                "provisions": ["one", "two"],
            },
            CONSUMER: {
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
    """Return the exact accepted check result for one host."""
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


def _state(controller: ModuleType, state_dir: Path) -> dict[str, Any]:
    """Return the whole state document one pass left behind."""
    return controller.load_state(state_dir / controller.STATE_FILE)


def _stranding(controller: ModuleType, state_dir: Path) -> dict[str, Any]:
    """Return the stranded-at-zero record the state file carries."""
    stored = _state(controller, state_dir).get("stranded")
    return stored if isinstance(stored, dict) else {}


def _receipts(controller: ModuleType, state_dir: Path) -> dict[str, Any]:
    """Return the receipts the state file carries after a pass."""
    stored = _state(controller, state_dir).get("hosts")
    return stored if isinstance(stored, dict) else {}


def _pass(  # noqa: PLR0913  # one pass's whole fixture: who drifts, fails, refuses
    controller: ModuleType,
    state_dir: Path,
    now: int,
    *,
    apply_fails: str = "",
    drift: str = "",
    refuse_drain: str = "",
    check_fails: str = "",
    mode: str = "apply",
) -> tuple[int, list[tuple[str, str]]]:
    """Run one pass over the fixture, recording every verb it issued."""
    data = _data()
    calls: list[tuple[str, str]] = []

    def fake_run(argv: Sequence[str]) -> frp.CommandResult:
        verb, host = _identity(argv)
        calls.append((verb, host))
        if verb in {"check", "parked-check"}:
            if host == check_fails:
                return frp.CommandResult(CHECK_FAILURE_STATUS, "", "ssh: host unreachable\n")
            if host == drift:
                base = controller.PRODUCER_CHECK_NOISE if host == PRODUCER else 0
                return _check(controller, data, host, base + 1)
            return _clean(controller, data, host)
        if verb == "parked-apply" and host == apply_fails:
            return frp.CommandResult(APPLY_FAILURE_STATUS, "", "runner image build failed\n")
        if verb == "quarantine" and host == refuse_drain:
            return frp.CommandResult(DRAIN_REFUSED_STATUS, "", "capacity API refused\n")
        return frp.CommandResult(0, "", "")

    status = controller.reconcile(
        data, _options(controller, state_dir, now, mode), fake_run, _no_wait
    )
    return status, calls


def _drain_producer(controller: ModuleType, state_dir: Path) -> int:
    """Drain the producer once through real drift, returning the next pass time."""
    _pass(controller, state_dir, FIRST_PASS, apply_fails=PRODUCER, drift=PRODUCER)
    return FIRST_PASS + PASS_INTERVAL


def _verbs(calls: Sequence[tuple[str, str]], host: str) -> list[str]:
    """Return the verbs one pass issued against ``host``, in order."""
    return [verb for verb, target in calls if target == host]


def _reopened_host_loses_its_record(controller: ModuleType, failures: list[str]) -> None:
    """A pass that restores and verifies capacity must stop counting the host at zero."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-serving-") as raw:
        state_dir = Path(raw)
        drained = _stranding(controller, state_dir)
        reopened_at = _drain_producer(controller, state_dir)
        held = _stranding(controller, state_dir).get(PRODUCER)
        first, calls = _pass(controller, state_dir, reopened_at, apply_fails=PRODUCER)
        record = _stranding(controller, state_dir)
        receipt = _receipts(controller, state_dir).get(CONSUMER)
        later, _next_calls = _pass(
            controller, state_dir, reopened_at + PASS_INTERVAL, apply_fails=PRODUCER
        )
        serving = controller.serving_hosts([PRODUCER, CONSUMER], _stranding(controller, state_dir))
    if drained:
        failures.append("the opening state file already carried a stranded-at-zero record")
    if not isinstance(held, dict) or held.get("passes") != 1:
        failures.append("the drifting pass did not record the producer it drained")
    if "restore" not in _verbs(calls, PRODUCER):
        failures.append(
            "the pass whose apply failed with no prior drift never reopened last-known-good "
            "capacity, so this case is not exercising the recovery path at all"
        )
    if PRODUCER in record:
        failures.append(
            "a producer whose capacity this pass REOPENED and verified is still recorded at "
            "ZERO, so the controller keeps counting passes a serving host is carrying work"
        )
    if first != 1:
        failures.append(f"a pass that reopened verified capacity exited {first}, not 1")
    if later == controller.STRANDED_STATUS:
        failures.append(
            "repeated recovery passes escalated STRANDED_STATUS over a host that was put "
            "back into service every single pass"
        )
    if isinstance(receipt, dict) and controller.RELEASED_RECEIPT_KEY in receipt:
        failures.append(
            "a consumer was marked PROVISIONAL against a producer that is serving its "
            "last-known-good image, so every later recovery re-applies the whole fleet"
        )
    if serving != [PRODUCER, CONSUMER]:
        failures.append(
            f"the hosts still serving came back as {serving}, so a reopened host stays "
            "exempt from the pass drain budget and can be taken to zero for free"
        )


def _host_still_at_zero_keeps_ageing(controller: ModuleType, failures: list[str]) -> None:
    """A record must still count the passes a host really does spend at zero."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-serving-") as raw:
        state_dir = Path(raw)
        now = _drain_producer(controller, state_dir)
        statuses = []
        for index in range(controller.STRANDED_ESCALATION_PASSES):
            status, _calls = _pass(
                controller, state_dir, now + index * PASS_INTERVAL, check_fails=PRODUCER
            )
            statuses.append(status)
        entry = _stranding(controller, state_dir).get(PRODUCER)
        receipt = _receipts(controller, state_dir).get(CONSUMER)
    if not isinstance(entry, dict):
        failures.append(
            "a producer left at zero by every pass lost its stranded-at-zero record, so "
            "nothing counts the passes this fleet is failing to recover in"
        )
        return
    if entry["passes"] != 1 + controller.STRANDED_ESCALATION_PASSES:
        failures.append(
            f"a host held at zero across {controller.STRANDED_ESCALATION_PASSES} read-only "
            f"failures counted {entry['passes']} pass(es)"
        )
    if entry["since"] != FIRST_PASS:
        failures.append("ageing a record moved the time the host was first drained")
    if controller.STRANDED_STATUS not in statuses:
        failures.append(
            "a host genuinely stranded at zero never escalated, so the narrow clear for a "
            "reopened host has silenced the escalation issue #888 needs"
        )
    if not isinstance(receipt, dict) or receipt.get(controller.RELEASED_RECEIPT_KEY) != PRODUCER:
        failures.append(
            "a consumer released past a producer still recorded at zero lost its "
            "PROVISIONAL mark, so the producer's recovery will not make it converge again"
        )


def _refused_drain_that_reopened_clears(controller: ModuleType, failures: list[str]) -> None:
    """A refused drain whose reopen verified leaves the host serving, so the record goes."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-serving-") as raw:
        state_dir = Path(raw)
        reopened_at = _drain_producer(controller, state_dir)
        status, calls = _pass(
            controller, state_dir, reopened_at, apply_fails=PRODUCER, refuse_drain=PRODUCER
        )
        record = _stranding(controller, state_dir)
    verbs = _verbs(calls, PRODUCER)
    if "quarantine" not in verbs or "restore" not in verbs:
        failures.append(f"the refused-drain case issued {verbs}, not a drain then a reopen")
    if verbs and verbs[-1] != "check":
        failures.append("the reopen was never verified, so this case proves nothing")
    if PRODUCER in record:
        failures.append(
            "a host whose drain was REFUSED and whose last-known-good capacity then "
            "verified is still recorded at zero while it is serving work"
        )
    if status != 1:
        failures.append(
            f"a refused drain with verified capacity exited {status}; the host is "
            "accounted for, so the pass is an ordinary failure"
        )


def _check_pass_rewrites_nothing(controller: ModuleType, failures: list[str]) -> None:
    """A check pass persists nothing, so it may not clear a record either."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-serving-") as raw:
        state_dir = Path(raw)
        now = _drain_producer(controller, state_dir)
        before = _state(controller, state_dir)
        _status, calls = _pass(
            controller, state_dir, now, apply_fails=PRODUCER, drift=PRODUCER, mode="check"
        )
        after = _state(controller, state_dir)
    mutations = [call for call in calls if controller.capacity_mutation(call[0])]
    if mutations:
        failures.append(f"a check pass issued capacity mutations {mutations}")
    if after.get("stranded") != before.get("stranded"):
        failures.append("a check pass rewrote the stranded-at-zero record it only reports on")
    if after.get("hosts") != before.get("hosts"):
        failures.append("a check pass rewrote the receipts it only reports on")


def _clear_is_narrow(controller: ModuleType, failures: list[str]) -> None:
    """Only a reopen verb clears, and only for a host that already had a record."""
    mutations = controller.CAPACITY_MUTATION_VERBS
    reopen = [verb for verb in mutations if controller.capacity_reopened(verb)]
    if sorted(reopen) != ["activate", "restore"]:
        failures.append(f"the verbs counted as reopening capacity are {sorted(reopen)}")
    if controller.capacity_reopened(""):
        failures.append("a transaction that issued no capacity verb claimed a reopen")
    if controller.capacity_reopened("quarantine"):
        failures.append("draining a host counted as putting it back into service")
    serving: dict[str, dict[str, int]] = {}
    if controller.clear_reopened_stranding(serving, PRODUCER):
        failures.append("a host with no record reported one cleared")
    if serving:
        failures.append("clearing invented a record for a host that never had one")
    at_zero = {PRODUCER: {"since": FIRST_PASS, "passes": 2}}
    if not controller.clear_reopened_stranding(at_zero, PRODUCER):
        failures.append("clearing a real stranded-at-zero record reported nothing cleared")
    if at_zero:
        failures.append(f"the record survived being cleared: {at_zero}")
    receipts: dict[str, Any] = {PRODUCER: {"checked_at": FIRST_PASS}}
    held = 2
    stranding = {PRODUCER: {"since": FIRST_PASS, "passes": held}}
    drained: list[str] = []
    controller.invalidate_receipt(
        receipts, stranding, drained, PRODUCER, FIRST_PASS, stranded=True, reopened=True
    )
    if PRODUCER not in stranding or stranding[PRODUCER]["passes"] != held + 1:
        failures.append(
            "a pass that took capacity down stopped recording the stranding because a "
            "reopen verb had been issued somewhere in the same transaction"
        )
    if receipts:
        failures.append("a failed host kept its receipt")


def run(controller: ModuleType) -> list[str]:
    """Return every failure about records kept against hosts back in service."""
    failures: list[str] = []
    _reopened_host_loses_its_record(controller, failures)
    _host_still_at_zero_keeps_ageing(controller, failures)
    _refused_drain_that_reopened_clears(controller, failures)
    _check_pass_rewrites_nothing(controller, failures)
    _clear_is_narrow(controller, failures)
    return failures
