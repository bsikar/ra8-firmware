# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Regression selftests for releases against a replaced producer (#888).

A consumer released onto a stranded producer's frozen last-known-good image
publishes a provisional receipt naming that producer, and the only thing that
ever expired it was the same producer being proven to serve again.  An operator
looking at a fleet the controller has been escalating for days does something
else: they point ``runner_image.source_host`` at a healthy host and retire the
one that has been at zero.  The named producer is then never the producer
again, so nothing clears the mark, every released consumer keeps a receipt that
looks freshly converged, and ``full_apply_due`` skips it for a whole interval
while the pass exits 0.  The fleet reports itself converged while running the
image the outage left behind and the new producer's image never reaches it,
which is the stale pinning of issue #888 arriving through the operator's fix
rather than through the original fault.

These tests pin the narrow expiry:

* a swapped-in producer makes the orphaned provisional receipt converge again,
  in the pass that finds the swap, and the pass no longer reports success;
* a host promoted from released consumer to producer sheds its own mark;
* a receipt naming the CURRENT producer is untouched, so the recovery path
  keeps its meaning while the producer is still the producer;
* ordinary receipts and hosts outside the declaration are left alone, and a
  check pass rewrites nothing.
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


def _data(producer: str) -> dict[str, Any]:
    """Return two interchangeable image hosts and one consumer."""
    return {
        "runner_image": {"source_host": producer},
        "hosts": {
            "image-a": {
                "class": "docker_linux",
                "runners": {"instances": 1},
                "provisions": ["one", "two"],
            },
            "image-b": {
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


def _receipts(controller: ModuleType, state_dir: Path) -> dict[str, Any]:
    """Return the receipts the state file carries after a pass."""
    document = controller.load_state(state_dir / controller.STATE_FILE)
    stored = document.get("hosts")
    return stored if isinstance(stored, dict) else {}


def _pass(  # noqa: PLR0913  # one pass's whole fixture: who produces, who fails, when
    controller: ModuleType,
    state_dir: Path,
    now: int,
    producer: str,
    *,
    strand: str = "",
    mode: str = "apply",
) -> tuple[int, list[tuple[str, str]]]:
    """Run one pass, optionally leaving ``strand`` unable to apply."""
    data = _data(producer)
    calls: list[tuple[str, str]] = []

    def fake_run(argv: Sequence[str]) -> frp.CommandResult:
        verb, host = _identity(argv)
        calls.append((verb, host))
        if verb in {"check", "parked-check"}:
            if host == strand:
                base = controller.PRODUCER_CHECK_NOISE if host == producer else 0
                return _check(controller, data, host, base + 1)
            return _clean(controller, data, host)
        if verb == "parked-apply" and host == strand:
            return frp.CommandResult(APPLY_FAILURE_STATUS, "", "runner image build failed\n")
        return frp.CommandResult(0, "", "")

    status = controller.reconcile(
        data, _options(controller, state_dir, now, mode), fake_run, _no_wait
    )
    return status, calls


def _release_onto(controller: ModuleType, state_dir: Path, producer: str) -> None:
    """Strand ``producer`` until its consumers are released onto its frozen image."""
    for index in range(controller.PRODUCER_BLOCK_PASSES):
        _pass(
            controller,
            state_dir,
            FIRST_PASS + index * PASS_INTERVAL,
            producer,
            strand=producer,
        )


def _applied(calls: Sequence[tuple[str, str]], host: str) -> bool:
    """Return whether one pass issued a parked apply against ``host``."""
    return ("parked-apply", host) in calls


def _swapped_producer_converges_the_fleet(controller: ModuleType, failures: list[str]) -> None:
    """Pointing the fleet at a healthy image host must reach its consumers."""
    after = FIRST_PASS + controller.PRODUCER_BLOCK_PASSES * PASS_INTERVAL
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-orphan-") as raw:
        state_dir = Path(raw)
        _release_onto(controller, state_dir, "image-a")
        status, calls = _pass(controller, state_dir, after, "image-b")
        receipts = _receipts(controller, state_dir)
    if not _applied(calls, "consumer"):
        failures.append(
            "the fleet's image producer was swapped to a healthy host and the consumer "
            "released onto the retired producer's frozen image was skipped as already "
            "converged, so the new producer's image never reaches the fleet"
        )
    if status:
        failures.append(
            f"the pass that repaired the pinning and converged every host exited {status}"
        )
    receipt = receipts.get("consumer")
    if not isinstance(receipt, dict):
        failures.append("the consumer lost its receipt on the pass that converged it")
        return
    if controller.RELEASED_RECEIPT_KEY in receipt:
        failures.append(
            "a consumer converged against the fleet's current producer kept a provisional "
            "mark naming a host that no longer publishes its image"
        )
    if receipt.get("full_applied_at") != after:
        failures.append("the re-converged consumer did not record this pass's full apply")


def _promoted_consumer_sheds_its_mark(controller: ModuleType, failures: list[str]) -> None:
    """A released consumer promoted to producer must not stay provisional."""
    after = FIRST_PASS + controller.PRODUCER_BLOCK_PASSES * PASS_INTERVAL
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-orphan-") as raw:
        state_dir = Path(raw)
        _release_onto(controller, state_dir, "image-a")
        _status, calls = _pass(controller, state_dir, after, "image-b")
        receipts = _receipts(controller, state_dir)
    if not _applied(calls, "image-b"):
        failures.append(
            "a host released onto the old producer's frozen image was promoted to "
            "producer and published from a receipt it earned while stranded"
        )
    receipt = receipts.get("image-b")
    if isinstance(receipt, dict) and controller.RELEASED_RECEIPT_KEY in receipt:
        failures.append("the promoted producer's own receipt is still marked provisional")


def _current_producer_keeps_its_releases(controller: ModuleType, failures: list[str]) -> None:
    """While the producer is still the producer, only its recovery may expire a mark."""
    total = controller.PRODUCER_BLOCK_PASSES
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-orphan-") as raw:
        state_dir = Path(raw)
        _release_onto(controller, state_dir, "image-a")
        _status, calls = _pass(
            controller,
            state_dir,
            FIRST_PASS + total * PASS_INTERVAL,
            "image-a",
            strand="image-a",
        )
        receipts = _receipts(controller, state_dir)
    if _applied(calls, "consumer"):
        failures.append(
            "a consumer released onto the CURRENT producer's frozen image was expired and "
            "re-applied while that producer was still the fleet's image source"
        )
    receipt = receipts.get("consumer")
    if not isinstance(receipt, dict) or receipt.get(controller.RELEASED_RECEIPT_KEY) != "image-a":
        failures.append(
            "the provisional mark against the still-stranded producer was dropped, so its "
            "recovery will no longer force the consumer to converge"
        )


def _healthy_fleet_expires_nothing(controller: ModuleType, failures: list[str]) -> None:
    """Nothing was ever released, so no pass may expire or re-apply anything."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-orphan-") as raw:
        state_dir = Path(raw)
        first, _calls = _pass(controller, state_dir, FIRST_PASS, "image-a")
        second, calls = _pass(controller, state_dir, FIRST_PASS + PASS_INTERVAL, "image-a")
        receipts = _receipts(controller, state_dir)
    if first or second:
        failures.append(f"healthy passes exited {first} and {second}")
    if _applied(calls, "consumer"):
        failures.append(
            "a healthy pass expired a receipt no release had ever marked, so every pass "
            "now re-applies the whole fleet"
        )
    receipt = receipts.get("consumer")
    if not isinstance(receipt, dict) or receipt.get("full_applied_at") != FIRST_PASS:
        failures.append("an ordinary receipt did not survive the next healthy pass intact")


def _check_pass_rewrites_nothing(controller: ModuleType, failures: list[str]) -> None:
    """A check pass persists nothing, so it may not expire a mark either."""
    after = FIRST_PASS + controller.PRODUCER_BLOCK_PASSES * PASS_INTERVAL
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-orphan-") as raw:
        state_dir = Path(raw)
        _release_onto(controller, state_dir, "image-a")
        before = _receipts(controller, state_dir)
        _status, calls = _pass(controller, state_dir, after, "image-b", mode="check")
        receipts = _receipts(controller, state_dir)
    mutations = [call for call in calls if controller.capacity_mutation(call[0])]
    if mutations:
        failures.append(f"a check pass issued capacity mutations {mutations}")
    if receipts != before:
        failures.append("a check pass rewrote the receipts it only had to report on")


def _expiry_is_narrow(controller: ModuleType, failures: list[str]) -> None:
    """Only a mark naming someone other than this fleet's producer may be dropped."""
    key = controller.RELEASED_RECEIPT_KEY
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-orphan-") as raw:
        options = _options(controller, Path(raw), FIRST_PASS)
        receipts = {
            "image-b": {"full_applied_at": FIRST_PASS, key: "image-a"},
            "consumer": {"full_applied_at": FIRST_PASS, key: "image-a"},
            "current": {"full_applied_at": FIRST_PASS, key: "image-b"},
            "plain": {"full_applied_at": FIRST_PASS},
            "retired": {"full_applied_at": FIRST_PASS, key: "image-a"},
        }
        order = ["image-b", "consumer", "current", "plain"]
        expired = controller.expire_orphaned_releases(receipts, order, options)
        if expired != ["image-b", "consumer"]:
            failures.append(f"expiry took {expired} instead of only the orphaned marks")
        if set(receipts) != {"current", "plain", "retired"}:
            failures.append("expiry dropped receipts it had no evidence to drop")
        if controller.expire_orphaned_releases(receipts, order, options):
            failures.append("expiry reported work on a fleet with nothing orphaned left")
        held = _options(controller, Path(raw), FIRST_PASS, "check")
        if controller.expire_orphaned_releases({"a": {key: "gone"}}, ["a"], held):
            failures.append("a check pass expired a provisional receipt")


def run(controller: ModuleType) -> list[str]:
    """Return every orphaned-release failure."""
    failures: list[str] = []
    _swapped_producer_converges_the_fleet(controller, failures)
    _promoted_consumer_sheds_its_mark(controller, failures)
    _current_producer_keeps_its_releases(controller, failures)
    _healthy_fleet_expires_nothing(controller, failures)
    _check_pass_rewrites_nothing(controller, failures)
    _expiry_is_narrow(controller, failures)
    return failures
