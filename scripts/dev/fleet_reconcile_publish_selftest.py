# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Regression selftests for releases a producer's republish must expire (#888).

A consumer released past a drained producer converges onto that producer's
FROZEN last-known-good image, so its receipt is marked provisional and names
the producer it was earned against.  Expiring those marks was gated on the
successful pass CLEARING the producer's stranded-at-zero record, on the
reasoning that climbing back off zero is what proves a republish.  A record is
also dropped by a pass that merely REOPENED last-known-good capacity, which is
what an apply that fails after a clean check does every time.  A producer whose
record went that way then republished with every release still marked against
it: nothing expired, ``full_apply_due`` skipped those consumers for a whole
interval, and the pass exited 0 with the fleet reported converged on the image
the outage left behind.

These tests pin the evidence being the marks rather than the record:

* a producer that reconciles after its record was cleared by a reopen pass
  still makes every consumer released against it converge again, in that pass;
* the original recovery path expires exactly as before, so a producer that
  climbs off zero is unchanged;
* a healthy fleet carries no marks, so it expires nothing and pays for no
  extra applies;
* only the current producer's own marks expire, and only when the producer is
  the host that reconciled;
* a check pass persists nothing at all.
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
OTHER_PRODUCER = "image-b"
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


def _pass(  # noqa: PLR0913  # one pass's whole fixture: who drifts, fails, is unreachable
    controller: ModuleType,
    state_dir: Path,
    now: int,
    *,
    apply_fails: str = "",
    drift: str = "",
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
        return frp.CommandResult(0, "", "")

    status = controller.reconcile(
        data, _options(controller, state_dir, now, mode), fake_run, _no_wait
    )
    return status, calls


def _verbs(calls: Sequence[tuple[str, str]], host: str) -> list[str]:
    """Return the verbs one pass issued against ``host``, in order."""
    return [verb for verb, target in calls if target == host]


def _release_consumer(controller: ModuleType, state_dir: Path) -> int:
    """Drain the producer until its consumer is released, returning the next pass time."""
    for index in range(controller.PRODUCER_BLOCK_PASSES):
        _pass(
            controller,
            state_dir,
            FIRST_PASS + index * PASS_INTERVAL,
            apply_fails=PRODUCER,
            drift=PRODUCER,
        )
    return FIRST_PASS + controller.PRODUCER_BLOCK_PASSES * PASS_INTERVAL


def _republish_after_a_reopen_expires_releases(controller: ModuleType, failures: list[str]) -> None:
    """A producer whose record a reopen pass cleared must still expire its releases."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-publish-") as raw:
        state_dir = Path(raw)
        reopened_at = _release_consumer(controller, state_dir)
        released = _receipts(controller, state_dir).get(CONSUMER)
        _reopen_status, reopen_calls = _pass(
            controller, state_dir, reopened_at, apply_fails=PRODUCER
        )
        record = _stranding(controller, state_dir)
        status, calls = _pass(controller, state_dir, reopened_at + PASS_INTERVAL)
        receipt = _receipts(controller, state_dir).get(CONSUMER)
    if not isinstance(released, dict) or released.get(controller.RELEASED_RECEIPT_KEY) != PRODUCER:
        failures.append(
            "the released consumer never earned a PROVISIONAL receipt naming the producer, "
            "so this case is not exercising the frozen-image release at all"
        )
        return
    if "restore" not in _verbs(reopen_calls, PRODUCER):
        failures.append(
            "the pass whose apply failed after a clean check never reopened last-known-good "
            "capacity, so the producer's record was not cleared the way this case needs"
        )
    if PRODUCER in record:
        failures.append("the reopen pass left the producer recorded at zero")
    if "parked-apply" not in _verbs(calls, CONSUMER):
        failures.append(
            "the producer republished and the consumer released against its frozen image was "
            "NOT made to converge again, so the fleet keeps running the pre-outage image while "
            "the pass reports it converged"
        )
    if not isinstance(receipt, dict):
        failures.append("the consumer lost its receipt entirely on the republishing pass")
        return
    if controller.RELEASED_RECEIPT_KEY in receipt:
        failures.append(
            "the consumer's receipt is still marked provisional against a producer that has "
            "republished, so every later pass expires it again"
        )
    if receipt.get("full_applied_at") != reopened_at + PASS_INTERVAL:
        failures.append(
            f"the re-converged consumer's receipt was stamped {receipt.get('full_applied_at')}, "
            "not the pass that actually applied the republished image"
        )
    if status:
        failures.append(f"the repair pass reported failure ({status}); every host converged")


def _recovery_from_a_live_record_still_expires(controller: ModuleType, failures: list[str]) -> None:
    """A producer that climbs back off zero expires its releases exactly as before."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-publish-") as raw:
        state_dir = Path(raw)
        recovered_at = _release_consumer(controller, state_dir)
        held = _stranding(controller, state_dir).get(PRODUCER)
        status, calls = _pass(controller, state_dir, recovered_at)
        receipt = _receipts(controller, state_dir).get(CONSUMER)
        record = _stranding(controller, state_dir)
    if not isinstance(held, dict):
        failures.append("the producer was never recorded at zero before its recovery pass")
    if "parked-apply" not in _verbs(calls, CONSUMER):
        failures.append(
            "a producer recovering straight from its stranded-at-zero record stopped making "
            "its released consumers converge again"
        )
    if isinstance(receipt, dict) and controller.RELEASED_RECEIPT_KEY in receipt:
        failures.append("the recovered producer's consumer kept its provisional mark")
    if PRODUCER in record:
        failures.append("the recovery pass left the producer recorded at zero")
    if status:
        failures.append(f"the recovery pass reported failure ({status}); every host converged")


def _healthy_fleet_expires_nothing(controller: ModuleType, failures: list[str]) -> None:
    """With no releases to expire, the extra expiry must cost the fleet no applies."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-publish-") as raw:
        state_dir = Path(raw)
        first, _first_calls = _pass(controller, state_dir, FIRST_PASS)
        second, calls = _pass(controller, state_dir, FIRST_PASS + PASS_INTERVAL)
        receipts = _receipts(controller, state_dir)
    if first or second:
        failures.append(f"a healthy fleet exited {first} then {second}")
    applies = [call for call in calls if call[0] == "parked-apply"]
    if applies:
        failures.append(
            f"the second healthy pass re-applied {applies}; nothing was released, so nothing "
            "should have been expired"
        )
    marked = [
        host for host, receipt in receipts.items() if controller.RELEASED_RECEIPT_KEY in receipt
    ]
    if marked:
        failures.append(f"a healthy fleet marked receipt(s) provisional: {marked}")


def _expiry_names_the_producer_that_published(controller: ModuleType, failures: list[str]) -> None:
    """Only the current producer's own marks expire, and only on the producer's success."""
    order = [PRODUCER, CONSUMER, "consumer-b"]
    receipt = {"checked_at": FIRST_PASS, "full_applied_at": FIRST_PASS, "source_digest": DIGEST}
    receipts: dict[str, Any] = {
        CONSUMER: {**receipt, controller.RELEASED_RECEIPT_KEY: PRODUCER},
        "consumer-b": {**receipt, controller.RELEASED_RECEIPT_KEY: OTHER_PRODUCER},
    }
    stranding: dict[str, dict[str, int]] = {}
    controller.record_success(
        receipts, stranding, order, PRODUCER, receipt, index=0, released=False
    )
    if CONSUMER in receipts:
        failures.append(
            "a producer with no stranded-at-zero record republished without expiring the "
            "release earned against its frozen image"
        )
    if receipts.get("consumer-b", {}).get(controller.RELEASED_RECEIPT_KEY) != OTHER_PRODUCER:
        failures.append(
            "a release naming another producer was expired by this producer's republish; that "
            "receipt is the orphan policy's to settle, not this one's"
        )
    if receipts.get(PRODUCER) != receipt:
        failures.append("the producer's own receipt was not published as an ordinary receipt")
    held: dict[str, Any] = {CONSUMER: {**receipt, controller.RELEASED_RECEIPT_KEY: PRODUCER}}
    controller.record_success(
        held, stranding, order, "consumer-b", receipt, index=2, released=False
    )
    if CONSUMER not in held:
        failures.append(
            "one consumer reconciling expired another consumer's provisional receipt; only the "
            "producer publishing the image can settle those"
        )


def _check_pass_rewrites_nothing(controller: ModuleType, failures: list[str]) -> None:
    """A check pass persists nothing, so it may not expire a release either."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-publish-") as raw:
        state_dir = Path(raw)
        now = _release_consumer(controller, state_dir)
        before = _state(controller, state_dir)
        _status, calls = _pass(controller, state_dir, now, mode="check")
        after = _state(controller, state_dir)
    mutations = [call for call in calls if controller.capacity_mutation(call[0])]
    if mutations:
        failures.append(f"a check pass issued capacity mutations {mutations}")
    if after.get("hosts") != before.get("hosts"):
        failures.append("a check pass rewrote the receipts it only reports on")
    if after.get("stranded") != before.get("stranded"):
        failures.append("a check pass rewrote the stranded-at-zero record it only reports on")


def run(controller: ModuleType) -> list[str]:
    """Return every failure about releases a producer's republish must expire."""
    failures: list[str] = []
    _republish_after_a_reopen_expires_releases(controller, failures)
    _recovery_from_a_live_record_still_expires(controller, failures)
    _healthy_fleet_expires_nothing(controller, failures)
    _expiry_names_the_producer_that_published(controller, failures)
    _check_pass_rewrites_nothing(controller, failures)
    return failures
