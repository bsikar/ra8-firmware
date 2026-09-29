# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Regression selftests for a fleet held at zero behind its producer (#888).

Issue #888's fleet did not just fall to zero capacity, it stayed there.  The
controller skips every consumer while the image producer has failed, so that a
consumer cannot converge onto an image being republished underneath it.  That
block had no bound at all: a producer drained on Friday held every consumer
back on every pass afterwards, so a consumer stranded at zero by an earlier
pass was skipped rather than repaired and the fleet could never climb out on
its own.  A drained producer publishes nothing, so once its stranding has
survived several passes the image is frozen at last-known-good and the
consumers would pull exactly what they already run.

These tests pin the bound:

* a producer stranded across ``PRODUCER_BLOCK_PASSES`` consecutive passes stops
  holding its consumers back, and the released consumer really does reconcile
  and publish a receipt;
* releasing the consumers never reports success, because the producer is still
  at zero;
* a producer that could not be DRAINED is unaccounted for and may still be
  publishing, so it keeps its consumers back for as long as it stays that way;
* a producer proven to be serving again restores the block, so the next
  stranding starts from one instead of inheriting a release already earned;
* the policy itself stays conservative: nothing released without a durable
  record of the producer sitting at zero.
"""

from __future__ import annotations

import tempfile
from collections.abc import Sequence
from pathlib import Path
from types import ModuleType
from typing import Any

import fleet_reconcile_process as frp

APPLY_FAILURE_STATUS = 1
DRAIN_REFUSED_STATUS = 7
ORDINARY_FAILURE_STATUS = 1
FIRST_PASS = 2000
PASS_INTERVAL = 100
FULL_INTERVAL = 100
PRODUCER_INTERVAL = 50
DIGEST = "b" * 64

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
    controller: ModuleType, state_dir: Path, now: int, *, drain_refused: bool = False
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
        if verb == "quarantine" and drain_refused:
            return frp.CommandResult(DRAIN_REFUSED_STATUS, "", "capacity API refused\n")
        return frp.CommandResult(0, "", "")

    status = controller.reconcile(data, _options(controller, state_dir, now), fake_run, _no_wait)
    return status, calls


def _healthy_pass(controller: ModuleType, state_dir: Path, now: int) -> int:
    """Run one pass that converges every host and reopens its capacity."""
    data = _data()

    def fake_run(argv: Sequence[str]) -> frp.CommandResult:
        verb, host = _identity(argv)
        if verb in {"check", "parked-check"}:
            return _clean(controller, data, host)
        return frp.CommandResult(0, "", "")

    return controller.reconcile(data, _options(controller, state_dir, now), fake_run, _no_wait)


def _touched(calls: list[tuple[str, str]], host: str) -> bool:
    """Return whether one pass ran any fleet verb against ``host``."""
    return any(target == host for _verb, target in calls)


def _durable_stranding_releases_consumers(controller: ModuleType, failures: list[str]) -> None:
    """A producer that is not recovering must stop holding the fleet at zero."""
    total = controller.PRODUCER_BLOCK_PASSES
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-release-") as raw:
        state_dir = Path(raw)
        runs = [
            _stranded_pass(controller, state_dir, FIRST_PASS + index * PASS_INTERVAL)
            for index in range(total)
        ]
        receipts = _receipts(controller, state_dir)
    for index, (_status, calls) in enumerate(runs[:-1]):
        if _touched(calls, "consumer"):
            failures.append(
                f"pass {index + 1} reconciled a consumer while the producer's stranding "
                "could still have been one transient failure mid-publish"
            )
    status, calls = runs[-1]
    if not _touched(calls, "consumer"):
        failures.append(
            f"a producer stranded at zero across {total} consecutive passes still "
            "blocked every consumer, so a consumer at zero can never be repaired"
        )
    if "consumer" not in receipts:
        failures.append("the released consumer never reconciled far enough to earn a receipt")
    if not status:
        failures.append(
            "a pass that released the consumers reported success though the producer "
            "is still sitting at zero capacity"
        )


def _undrained_producer_keeps_the_block(controller: ModuleType, failures: list[str]) -> None:
    """A producer that could not be drained may still publish: keep blocking."""
    passes = controller.PRODUCER_BLOCK_PASSES + 1
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-release-") as raw:
        state_dir = Path(raw)
        runs = [
            _stranded_pass(
                controller, state_dir, FIRST_PASS + index * PASS_INTERVAL, drain_refused=True
            )
            for index in range(passes)
        ]
    if any(_touched(calls, "consumer") for _status, calls in runs):
        failures.append(
            "an unaccounted-for producer released its consumers; it was never drained, "
            "so it may still be republishing the image underneath them"
        )
    if any(status != controller.DRAIN_FAILED_STATUS for status, _calls in runs):
        failures.append("a refused drain lost its unaccounted-for verdict across repeated passes")


def _recovery_restores_the_block(controller: ModuleType, failures: list[str]) -> None:
    """A producer proven to be serving again starts its next stranding from one."""
    total = controller.PRODUCER_BLOCK_PASSES
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-release-") as raw:
        state_dir = Path(raw)
        for index in range(total):
            _stranded_pass(controller, state_dir, FIRST_PASS + index * PASS_INTERVAL)
        healthy = _healthy_pass(controller, state_dir, FIRST_PASS + total * PASS_INTERVAL)
        status, calls = _stranded_pass(
            controller, state_dir, FIRST_PASS + (total + 1) * PASS_INTERVAL
        )
    if healthy:
        failures.append(f"a pass that reconciled every host exited {healthy}")
    if _touched(calls, "consumer"):
        failures.append(
            "a fresh stranding released the consumers on its first pass, inheriting a "
            "release earned before the producer recovered"
        )
    if status != ORDINARY_FAILURE_STATUS:
        failures.append(f"a fresh stranding after a recovery exited {status}")


def _release_policy_stays_conservative(controller: ModuleType, failures: list[str]) -> None:
    """Nothing is released without a durable record of a drained producer."""
    if controller.consumers_released({}, "producer", undrained=False):
        failures.append("consumers were released with no record of the producer at zero")
    early = {"producer": {"since": FIRST_PASS, "passes": controller.PRODUCER_BLOCK_PASSES - 1}}
    if controller.consumers_released(early, "producer", undrained=False):
        failures.append("consumers were released before the producer's stranding was durable")
    durable = {"producer": {"since": FIRST_PASS, "passes": controller.PRODUCER_BLOCK_PASSES}}
    if not controller.consumers_released(durable, "producer", undrained=False):
        failures.append("a producer durably stranded at zero still held every consumer back")
    if controller.consumers_released(durable, "producer", undrained=True):
        failures.append("a producer that could not be drained released its consumers")
    blocking, released = controller.producer_block_state(
        durable, "producer", stranded=True, undrained=True
    )
    if not blocking or released:
        failures.append("a producer that could not be drained stopped holding its consumers back")
    blocking, released = controller.producer_block_state(
        {}, "producer", stranded=False, undrained=False
    )
    if blocking or released:
        failures.append("the producer block ignored what the pass actually did to capacity")
    blocking, released = controller.producer_block_state(
        durable, "producer", stranded=False, undrained=False
    )
    if blocking or not released:
        failures.append(
            "a producer already recorded at zero released its consumers onto a frozen image "
            "without making their receipts provisional"
        )


def run(controller: ModuleType) -> list[str]:
    """Return every bounded-producer-block failure."""
    failures: list[str] = []
    _durable_stranding_releases_consumers(controller, failures)
    _undrained_producer_keeps_the_block(controller, failures)
    _recovery_restores_the_block(controller, failures)
    _release_policy_stays_conservative(controller, failures)
    return failures
