# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Regression selftests for receipts earned against a frozen image (#888).

Once a producer has sat at zero capacity for several consecutive passes the
controller releases its consumers, because a drained producer publishes
nothing and its image is therefore frozen at last-known-good.  Those consumers
reconcile and publish an ordinary receipt stamped ``full_applied_at``, which is
a claim about convergence the pass cannot honestly make: the consumer converged
onto the image from before the outage.  Nothing distinguished that receipt from
one earned against a healthy producer, so when the producer finally recovered
and republished, every consumer already looked freshly converged and was
skipped for a whole interval.  The fleet reported itself fully converged while
running the image the outage left behind, and the recovery never reached it,
which is the other half of the way issue #888's fleet failed to climb out.

These tests pin the narrow fix:

* a receipt earned during a released pass is marked provisional and names the
  producer whose frozen image it was converged against;
* the marker survives a later released pass that finds the consumer current,
  so it cannot be quietly laundered into an ordinary receipt;
* a producer proven to be serving again expires those receipts and the
  consumer really is converged again in that same pass;
* an ordinary healthy pass expires nothing, so the fix costs no extra applies
  when nothing was ever released.
"""

from __future__ import annotations

import tempfile
from collections.abc import Sequence
from pathlib import Path
from types import ModuleType
from typing import Any

import fleet_reconcile_process as frp

APPLY_FAILURE_STATUS = 1
FIRST_PASS = 5000
PASS_INTERVAL = 10
FULL_INTERVAL = 1000
PRODUCER_INTERVAL = 500
DIGEST = "c" * 64

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
    """Return the exact accepted check result for one host."""
    producer = host == data["runner_image"]["source_host"]
    return _check(controller, data, host, controller.PRODUCER_CHECK_NOISE if producer else 0)


def _options(controller: ModuleType, state_dir: Path, now: int) -> object:
    """Return deterministic apply-mode policy for one pass at ``now``.

    The controller arrives as a module, so its option dataclass is only handed
    straight back to it and never inspected in this file.
    """
    return controller.ReconcileOptions(
        mode="apply",
        force=False,
        source_digest=DIGEST,
        state_dir=state_dir,
        full_interval=FULL_INTERVAL,
        producer_interval=PRODUCER_INTERVAL,
        now=now,
    )


def _no_wait(_seconds: float) -> None:
    """Take retry pacing out of these cases without skipping it."""


def _receipts(controller: ModuleType, state_dir: Path) -> dict[str, Any]:
    """Return the receipts the state file carries after a pass."""
    document = controller.load_state(state_dir / controller.STATE_FILE)
    stored = document.get("hosts")
    return stored if isinstance(stored, dict) else {}


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


def _healthy_pass(
    controller: ModuleType, state_dir: Path, now: int
) -> tuple[int, list[tuple[str, str]]]:
    """Run one pass that converges every host and reopens its capacity."""
    data = _data()
    calls: list[tuple[str, str]] = []

    def fake_run(argv: Sequence[str]) -> frp.CommandResult:
        verb, host = _identity(argv)
        calls.append((verb, host))
        if verb in {"check", "parked-check"}:
            return _clean(controller, data, host)
        return frp.CommandResult(0, "", "")

    status = controller.reconcile(data, _options(controller, state_dir, now), fake_run, _no_wait)
    return status, calls


def _applied(calls: list[tuple[str, str]], host: str) -> bool:
    """Return whether one pass issued a parked apply against ``host``."""
    return ("parked-apply", host) in calls


def _release_sequence(controller: ModuleType, state_dir: Path) -> dict[str, Any]:
    """Strand the producer until its consumers are released, once."""
    for index in range(controller.PRODUCER_BLOCK_PASSES):
        _stranded_pass(controller, state_dir, FIRST_PASS + index * PASS_INTERVAL)
    return _receipts(controller, state_dir)


def _released_receipt_is_provisional(controller: ModuleType, failures: list[str]) -> None:
    """A consumer released onto a frozen image must say so in its receipt."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-freeze-") as raw:
        receipts = _release_sequence(controller, Path(raw))
    receipt = receipts.get("consumer")
    if not isinstance(receipt, dict):
        failures.append("the released consumer never reconciled far enough to earn a receipt")
        return
    if receipt.get(controller.RELEASED_RECEIPT_KEY) != "producer":
        failures.append(
            "a consumer reconciled against the producer's frozen last-known-good image "
            "published a receipt indistinguishable from real convergence, so the "
            "producer's recovery will never be applied to it"
        )
    if not isinstance(receipt.get("full_applied_at"), int):
        failures.append("marking a released receipt lost the convergence it does record")


def _marker_survives_a_quiet_released_pass(controller: ModuleType, failures: list[str]) -> None:
    """A later released pass that finds the consumer current keeps the mark."""
    total = controller.PRODUCER_BLOCK_PASSES
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-freeze-") as raw:
        state_dir = Path(raw)
        _release_sequence(controller, state_dir)
        _status, calls = _stranded_pass(controller, state_dir, FIRST_PASS + total * PASS_INTERVAL)
        receipts = _receipts(controller, state_dir)
    if _applied(calls, "consumer"):
        failures.append("a released consumer that was already current was applied again")
    receipt = receipts.get("consumer")
    if not isinstance(receipt, dict) or receipt.get(controller.RELEASED_RECEIPT_KEY) != "producer":
        failures.append(
            "a quiet pass laundered a provisional receipt into an ordinary one while the "
            "producer was still sitting at zero"
        )


def _recovery_converges_released_consumers(controller: ModuleType, failures: list[str]) -> None:
    """A producer serving again must reach the consumers it left behind."""
    total = controller.PRODUCER_BLOCK_PASSES
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-freeze-") as raw:
        state_dir = Path(raw)
        _release_sequence(controller, state_dir)
        status, calls = _healthy_pass(controller, state_dir, FIRST_PASS + total * PASS_INTERVAL)
        receipts = _receipts(controller, state_dir)
    if not _applied(calls, "consumer"):
        failures.append(
            "a recovered producer republished its image and the consumer released onto the "
            "frozen one was skipped as already converged, so the fleet keeps running the "
            "image the outage left behind"
        )
    if status:
        failures.append(f"a pass that reconciled every host exited {status}")
    receipt = receipts.get("consumer")
    if not isinstance(receipt, dict):
        failures.append("the consumer lost its receipt on the pass that converged it")
        return
    if controller.RELEASED_RECEIPT_KEY in receipt:
        failures.append(
            "a consumer converged against a serving producer kept its provisional mark, so "
            "it would be forced through a full apply on every later recovery"
        )


def _healthy_fleet_expires_nothing(controller: ModuleType, failures: list[str]) -> None:
    """Nothing was ever released, so nothing may be expired."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-freeze-") as raw:
        state_dir = Path(raw)
        first, _calls = _healthy_pass(controller, state_dir, FIRST_PASS)
        second, calls = _healthy_pass(controller, state_dir, FIRST_PASS + PASS_INTERVAL)
        receipts = _receipts(controller, state_dir)
    if first or second:
        failures.append(f"healthy passes exited {first} and {second}")
    if _applied(calls, "consumer"):
        failures.append(
            "a healthy pass expired a receipt no release had ever made provisional, so every "
            "pass now re-applies the whole fleet"
        )
    receipt = receipts.get("consumer")
    if not isinstance(receipt, dict) or receipt.get("full_applied_at") != FIRST_PASS:
        failures.append("an ordinary receipt did not survive the next healthy pass intact")


def _expiry_policy_is_narrow(controller: ModuleType, failures: list[str]) -> None:
    """Only this producer's provisional receipts may be expired."""
    marked = controller.released_receipt({"full_applied_at": FIRST_PASS}, "consumer", "producer")
    if marked.get("full_applied_at") != FIRST_PASS:
        failures.append("marking a receipt dropped what it already recorded")
    receipts = {
        "plain": {"full_applied_at": FIRST_PASS},
        "other": {controller.RELEASED_RECEIPT_KEY: "elsewhere"},
        "mine": dict(marked),
    }
    expired = controller.expire_released_receipts(receipts, ["plain", "other", "mine"], "producer")
    if expired != ["mine"]:
        failures.append(f"expiry took {expired} instead of only this producer's released receipt")
    if set(receipts) != {"plain", "other"}:
        failures.append("expiry dropped receipts it had no evidence to drop")
    if controller.expire_released_receipts(receipts, ["plain", "other"], "producer"):
        failures.append("expiry reported work on a fleet that had nothing provisional left")
    durable = {"producer": {"since": FIRST_PASS, "passes": controller.PRODUCER_BLOCK_PASSES}}
    early = {"producer": {"since": FIRST_PASS, "passes": controller.PRODUCER_BLOCK_PASSES - 1}}
    states = {
        "durable": controller.producer_block_state(
            durable, "producer", stranded=True, undrained=False
        ),
        "early": controller.producer_block_state(early, "producer", stranded=True, undrained=False),
        "undrained": controller.producer_block_state(
            durable, "producer", stranded=True, undrained=True
        ),
        "intact": controller.producer_block_state(
            durable, "producer", stranded=False, undrained=False
        ),
    }
    expected = {
        "durable": (False, True),
        "early": (True, False),
        "undrained": (True, False),
        "intact": (False, False),
    }
    for name, state in states.items():
        if state != expected[name]:
            failures.append(f"producer_block_state({name}) returned {state}")


def _stranding_record_reports_recovery(controller: ModuleType, failures: list[str]) -> None:
    """Clearing a stranding must report whether the host was held at zero."""
    stranding = {"producer": {"since": FIRST_PASS, "passes": 1}}
    if not controller.clear_stranding(stranding, "producer"):
        failures.append("a host climbing back off zero capacity was not reported as recovering")
    if controller.clear_stranding(stranding, "producer"):
        failures.append("an ordinary successful pass claimed the host had been at zero")


def run(controller: ModuleType) -> list[str]:
    """Return every frozen-image receipt failure."""
    failures: list[str] = []
    _released_receipt_is_provisional(controller, failures)
    _marker_survives_a_quiet_released_pass(controller, failures)
    _recovery_converges_released_consumers(controller, failures)
    _healthy_fleet_expires_nothing(controller, failures)
    _expiry_policy_is_narrow(controller, failures)
    _stranding_record_reports_recovery(controller, failures)
    return failures
