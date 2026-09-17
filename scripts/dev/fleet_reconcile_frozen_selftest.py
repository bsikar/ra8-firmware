# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Regression selftests for releases past a producer already at zero (#888).

A producer whose failure cost no capacity does not block its consumers: a
read-only check mutates nothing, so the image they depend on did not move and
holding them back would strand them for a fault that touched nothing.  That
carve-out said nothing about whether the producer is SERVING.  A producer an
earlier pass drained carries a stranded-at-zero record and publishes nothing,
so the everyday shape of a host that is already down, a check that keeps
failing against it, released every consumer onto its FROZEN last-known-good
image and let them publish ORDINARY receipts stamped ``full_applied_at``.
Nothing marked those receipts provisional, so the producer's eventual recovery
expired nothing, ``full_apply_due`` skipped the consumers for a whole interval,
and the pass exited 0 with the fleet reported converged on the image the outage
left behind.  The deliberate release is marked for exactly this reason; this
path reached the same stale pinning without ever crossing
``PRODUCER_BLOCK_PASSES``.

These tests pin the narrow mark:

* a consumer that converges while the producer is recorded at zero holds a
  PROVISIONAL receipt, and the producer's recovery makes it converge again in
  that same pass;
* a producer still SERVING keeps the ordinary receipts its consumers earn, so
  a failed read-only check costs the fleet no extra applies;
* a consumer merely found current during such a pass cannot launder its
  receipt into an unmarked one;
* a check pass persists nothing, and a refused drain proves nothing about what
  a host serves, so neither one may claim a frozen image.
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


def _receipts(controller: ModuleType, state_dir: Path) -> dict[str, Any]:
    """Return the receipts the state file carries after a pass."""
    stored = _state(controller, state_dir).get("hosts")
    return stored if isinstance(stored, dict) else {}


def _pass(  # noqa: PLR0913  # one pass's whole fixture: who drifts, who fails, when
    controller: ModuleType,
    state_dir: Path,
    now: int,
    *,
    drain: str = "",
    check_fails: str = "",
    mode: str = "apply",
) -> tuple[int, list[tuple[str, str]]]:
    """Run one pass, optionally draining ``drain`` or failing ``check_fails``'s check."""
    data = _data()
    calls: list[tuple[str, str]] = []

    def fake_run(argv: Sequence[str]) -> frp.CommandResult:
        verb, host = _identity(argv)
        calls.append((verb, host))
        if verb in {"check", "parked-check"}:
            if host == check_fails:
                return frp.CommandResult(CHECK_FAILURE_STATUS, "", "ssh: host unreachable\n")
            if host == drain:
                base = controller.PRODUCER_CHECK_NOISE if host == PRODUCER else 0
                return _check(controller, data, host, base + 1)
            return _clean(controller, data, host)
        if verb == "parked-apply" and host == drain:
            return frp.CommandResult(APPLY_FAILURE_STATUS, "", "runner image build failed\n")
        return frp.CommandResult(0, "", "")

    status = controller.reconcile(
        data, _options(controller, state_dir, now, mode), fake_run, _no_wait
    )
    return status, calls


def _strand_producer(controller: ModuleType, state_dir: Path, passes: int) -> int:
    """Drain the producer for ``passes`` consecutive passes, returning the next pass time."""
    for index in range(passes):
        _pass(controller, state_dir, FIRST_PASS + index * PASS_INTERVAL, drain=PRODUCER)
    return FIRST_PASS + passes * PASS_INTERVAL


def _applied(calls: Sequence[tuple[str, str]], host: str) -> bool:
    """Return whether one pass issued a parked apply against ``host``."""
    return ("parked-apply", host) in calls


def _drained_producer_marks_its_releases(controller: ModuleType, failures: list[str]) -> None:
    """A consumer converging while the producer sits at zero must stay provisional."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-frozen-") as raw:
        state_dir = Path(raw)
        released_at = _strand_producer(controller, state_dir, 2)
        status, calls = _pass(controller, state_dir, released_at, check_fails=PRODUCER)
        receipt = _receipts(controller, state_dir).get(CONSUMER)
        recovered, repair = _pass(controller, state_dir, released_at + PASS_INTERVAL)
        after = _receipts(controller, state_dir).get(CONSUMER)
    if not _applied(calls, CONSUMER):
        failures.append(
            "a read-only check failure against an already-drained producer did not let the "
            "consumer reconcile, so it stays at zero behind a producer that is not recovering"
        )
    if not status:
        failures.append("a pass that left the producer at zero capacity reported success")
    if not isinstance(receipt, dict):
        failures.append("the consumer earned no receipt from the pass that converged it")
        return
    if receipt.get(controller.RELEASED_RECEIPT_KEY) != PRODUCER:
        failures.append(
            "a consumer converged against a drained producer's FROZEN image published an "
            "ordinary receipt, so the producer's recovery will never make it converge again"
        )
    if not _applied(repair, CONSUMER):
        failures.append(
            "the producer republished its image and the consumer was skipped as already "
            "converged, so the fleet keeps running the image the outage left behind"
        )
    if recovered:
        failures.append(f"the pass that repaired the pinning exited {recovered}")
    if not isinstance(after, dict) or after.get("full_applied_at") != released_at + PASS_INTERVAL:
        failures.append("the re-converged consumer did not record the repair pass's full apply")
    elif controller.RELEASED_RECEIPT_KEY in after:
        failures.append("a consumer converged against a serving producer is still provisional")


def _serving_producer_keeps_ordinary_receipts(controller: ModuleType, failures: list[str]) -> None:
    """A producer that failed while still serving costs the fleet no extra applies."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-frozen-") as raw:
        state_dir = Path(raw)
        _status, calls = _pass(controller, state_dir, FIRST_PASS, check_fails=PRODUCER)
        receipt = _receipts(controller, state_dir).get(CONSUMER)
        _next_status, later = _pass(controller, state_dir, FIRST_PASS + PASS_INTERVAL)
    if not _applied(calls, CONSUMER):
        failures.append(
            "a read-only producer check failure blocked the consumer even though nothing "
            "was mutated and the image it depends on never moved"
        )
    if not isinstance(receipt, dict):
        failures.append("the consumer earned no receipt past a producer that kept serving")
        return
    if controller.RELEASED_RECEIPT_KEY in receipt:
        failures.append(
            "a consumer that converged while the producer was still SERVING was marked "
            "provisional, so every producer recovery now re-applies the whole fleet"
        )
    if _applied(later, CONSUMER):
        failures.append("the next pass re-applied a consumer whose receipt proves the image")


def _current_consumer_cannot_launder_its_mark(controller: ModuleType, failures: list[str]) -> None:
    """Being found current during a frozen-image pass must not clear the mark."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-frozen-") as raw:
        state_dir = Path(raw)
        healthy, _calls = _pass(controller, state_dir, FIRST_PASS)
        _pass(controller, state_dir, FIRST_PASS + PASS_INTERVAL, drain=PRODUCER)
        _status, calls = _pass(
            controller, state_dir, FIRST_PASS + 2 * PASS_INTERVAL, check_fails=PRODUCER
        )
        receipt = _receipts(controller, state_dir).get(CONSUMER)
    if healthy:
        failures.append(f"the opening healthy pass exited {healthy}")
    if _applied(calls, CONSUMER):
        failures.append("a consumer already current was re-applied for no drift")
    if not isinstance(receipt, dict):
        failures.append("a consumer found current lost the receipt it already held")
        return
    if receipt.get(controller.RELEASED_RECEIPT_KEY) != PRODUCER:
        failures.append(
            "a consumer found current while the producer sat at zero kept an unmarked "
            "receipt, so the producer's recovery will not make it converge again"
        )


def _check_pass_claims_nothing(controller: ModuleType, failures: list[str]) -> None:
    """A check pass persists nothing, so it may not mark or mutate anything."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-frozen-") as raw:
        state_dir = Path(raw)
        released_at = _strand_producer(controller, state_dir, 2)
        before = _receipts(controller, state_dir)
        _status, calls = _pass(
            controller, state_dir, released_at, check_fails=PRODUCER, mode="check"
        )
        receipts = _receipts(controller, state_dir)
    mutations = [call for call in calls if controller.capacity_mutation(call[0])]
    if mutations:
        failures.append(f"a check pass issued capacity mutations {mutations}")
    if receipts != before:
        failures.append("a check pass rewrote the receipts it only had to report on")


def _frozen_release_is_narrow(controller: ModuleType, failures: list[str]) -> None:
    """Only a producer this controller drained and never reopened has a frozen image."""
    serving: dict[str, dict[str, int]] = {}
    at_zero = {PRODUCER: {"since": FIRST_PASS, "passes": 1}}
    if controller.frozen_image_release(serving, PRODUCER, undrained=False):
        failures.append("a producer with no stranded-at-zero record claimed a frozen image")
    if not controller.frozen_image_release(at_zero, PRODUCER, undrained=False):
        failures.append("a producer recorded at zero did not freeze the image it publishes")
    if controller.frozen_image_release(at_zero, PRODUCER, undrained=True):
        failures.append(
            "a producer whose drain was REFUSED claimed a frozen image; nothing proved it "
            "had stopped publishing, so its consumers must stay blocked"
        )
    if controller.producer_block_state(serving, PRODUCER, stranded=False, undrained=False) != (
        False,
        False,
    ):
        failures.append("a producer that failed while serving no longer releases cleanly")
    if controller.producer_block_state(at_zero, PRODUCER, stranded=False, undrained=False) != (
        False,
        True,
    ):
        failures.append("a producer already at zero did not release its consumers provisionally")
    blocking, released = controller.producer_block_state(
        at_zero, PRODUCER, stranded=True, undrained=False
    )
    if not blocking or released:
        failures.append(
            "a producer drained this pass stopped blocking before "
            f"{controller.PRODUCER_BLOCK_PASSES} passes"
        )


def run(controller: ModuleType) -> list[str]:
    """Return every frozen-image release failure."""
    failures: list[str] = []
    _drained_producer_marks_its_releases(controller, failures)
    _serving_producer_keeps_ordinary_receipts(controller, failures)
    _current_consumer_cannot_launder_its_mark(controller, failures)
    _check_pass_claims_nothing(controller, failures)
    _frozen_release_is_narrow(controller, failures)
    return failures
