# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Selftests for reading the ARC activation check's apply-required status.

The activation check is a ``--check`` run, so the verifier reports work still
outstanding by exit status: ``fw.APPLY_REQUIRED_STATUS`` means the check RAN
and found one change, which is why ``inspect_host`` counts it as a clean read
of one drifted task.  The activation check read it as a failed check instead,
which made the sole ARC capacity opener unusable on exactly the host that
expects an outstanding change: an ARC producer is checked against
``PRODUCER_HELD_CHECK_NOISE`` while admission is held at zero, so the status
reporting that one change quarantined the host, the apply failed, and the pass
recorded it at zero.  The recovery hook reopens through the same activation
sequence, so nothing could lift it off zero either: an ARC scale set still
declared for all its runners with none of them in service, drained again every
pass and escalating for ever, with the consumers released onto its frozen
image once the record crossed ``PRODUCER_BLOCK_PASSES`` (issue #888, and the
dry-run evidence in it).

These tests pin the read and its narrowness:

* an ARC producer whose activation check reports its one held change that way
  is opened rather than quarantined, and the pass converges;
* repeated passes never strand it, never escalate, and leave the consumers
  ordinary receipts instead of provisional ones;
* a genuine activation-check failure still holds the host at zero;
* a host whose held expectation is zero still quarantines on an outstanding
  change, so the status is counted and not waved through;
* recovery reopens last-known-good ARC capacity through the same read;
* the status mapping is pinned at unit level against a clean recap, a genuine
  failure and a malformed recap.
"""

from __future__ import annotations

import contextlib
import io
import tempfile
from collections.abc import Sequence
from pathlib import Path
from types import ModuleType
from typing import Any

import fleet_reconcile_process as frp
import fleet_wsl as fw

DIGEST = "c" * 64
FIRST_PASS = 6000
PASS_STRIDE = 10
STALE_APPLIED_AT = 100
FRESH_APPLIED_AT = 5990
FULL_INTERVAL = 100
PRODUCER_INTERVAL = 50
DECLARED_RUNNERS = 6
GENUINE_CHECK_FAILURE = 2
REPEATED_PASSES = 4
ORDINARY_FAILURE_STATUS = 1

_VERBS = {
    "reconcile-parked-apply": "parked-apply",
    "reconcile-parked-check": "parked-check",
    "reconcile-activate": "activate",
    "reconcile-activation-check": "activation-check",
    "capacity-quarantine": "quarantine",
    "capacity-restore": "restore",
}


def _data(*, arc_producer: bool = True) -> dict[str, Any]:
    """Return a two-host fleet with the ARC scale set on one side or the other."""
    producer_class = "arc_k8s" if arc_producer else "docker_linux"
    consumer_class = "docker_linux" if arc_producer else "arc_k8s"
    return {
        "runner_image": {"source_host": "producer"},
        "hosts": {
            "producer": {
                "class": producer_class,
                "runners": {"instances": DECLARED_RUNNERS},
                "provisions": ["one", "two"],
            },
            "consumer": {
                "class": consumer_class,
                "runners": {"instances": DECLARED_RUNNERS},
                "provisions": ["one"],
            },
        },
    }


def _identity(argv: Sequence[str]) -> tuple[str, str]:
    """Return the reconciler verb and host from fleet argv."""
    return _VERBS.get(argv[2], argv[2]), argv[-1]


def _recap(
    controller: ModuleType, data: dict[str, Any], host: str, changed: int
) -> frp.CommandResult:
    """Return one successful check whose first play reports ``changed``."""
    name = controller.recap_identity(data, host)
    row = f"{name} : ok=9 changed={{}} unreachable=0 failed=0 skipped=1 rescued=0 ignored=0\n"
    plays = len(data["hosts"][host]["provisions"])
    return frp.CommandResult(0, row.format(changed) + row.format(0) * (plays - 1), "")


def _no_wait(_seconds: float) -> None:
    """Take retry pacing out of these cases without skipping it."""


class _Fleet:
    """One ARC host modelled by what its live ceiling actually does.

    ``restore`` is the sole ARC capacity opener and it only lifts the ceiling
    while the ARC authority is declared, which is why ``activate`` and its
    held-zero check come first; a parked apply tears that declaration down.
    """

    def __init__(
        self,
        controller: ModuleType,
        data: dict[str, Any],
        *,
        arc_host: str,
        activation_status: int,
        apply_fails: bool = False,
    ) -> None:
        """Start the fleet serving its declared runners."""
        self.controller = controller
        self.data = data
        self.arc_host = arc_host
        self.activation_status = activation_status
        self.apply_fails = apply_fails
        self.calls: list[tuple[str, str]] = []
        self.live_scale = DECLARED_RUNNERS
        self.declared = True

    def _arc_capacity(self, verb: str) -> None:
        """Move the modelled ARC ceiling the way the real one moves."""
        if verb == "parked-apply":
            self.live_scale = 0
            self.declared = False
        elif verb == "quarantine":
            self.live_scale = 0
        elif verb == "activate":
            self.declared = True
        elif verb == "restore" and self.declared:
            self.live_scale = DECLARED_RUNNERS

    def _parked_noise(self, host: str) -> int:
        """Return the change count one host's parked check is expected to report.

        Only an ARC producer is checked against the held-zero expectation,
        because only its capacity is opened by declaring an authority the
        parked apply tears down.
        """
        if host != "producer":
            return 0
        if host == self.arc_host:
            return int(self.controller.PRODUCER_HELD_CHECK_NOISE)
        return int(self.controller.PRODUCER_CHECK_NOISE)

    def run(self, argv: Sequence[str]) -> frp.CommandResult:
        """Answer one fleet command, moving the modelled capacity with it."""
        verb, host = _identity(argv)
        self.calls.append((verb, host))
        if host == self.arc_host:
            self._arc_capacity(verb)
            if verb == "activation-check" and self.activation_status:
                return frp.CommandResult(
                    self.activation_status, "", "arc authority: apply required\n"
                )
        if verb == "parked-apply" and self.apply_fails:
            return frp.CommandResult(1, "", "curl: (28) operation timed out\n")
        if verb == "activation-check":
            return _recap(
                self.controller, self.data, host, self.controller.PRODUCER_HELD_CHECK_NOISE
            )
        if verb == "parked-check":
            return _recap(self.controller, self.data, host, self._parked_noise(host))
        if verb == "check":
            noise = self.controller.PRODUCER_CHECK_NOISE if host == "producer" else 0
            return _recap(self.controller, self.data, host, noise)
        return frp.CommandResult(0, "", "")

    def verbs(self, host: str) -> list[str]:
        """Return the verbs issued against one host, in order."""
        return [verb for verb, target in self.calls if target == host]


def _options(controller: ModuleType, state_dir: Path, now: int) -> object:
    """Return apply-mode options whose producer is due a full verification."""
    return controller.ReconcileOptions(
        mode="apply",
        force=False,
        source_digest=DIGEST,
        state_dir=state_dir,
        full_interval=FULL_INTERVAL,
        producer_interval=PRODUCER_INTERVAL,
        now=now,
    )


def _seed(controller: ModuleType, state_dir: Path, *, consumer_due: bool) -> None:
    """Write receipts leaving the hosts under test due a periodic full verification."""
    consumer_applied_at = STALE_APPLIED_AT if consumer_due else FRESH_APPLIED_AT
    controller.save_state(
        state_dir / controller.STATE_FILE,
        {
            "version": 1,
            "hosts": {
                "producer": {
                    "source_digest": DIGEST,
                    "checked_at": STALE_APPLIED_AT,
                    "full_applied_at": STALE_APPLIED_AT,
                },
                "consumer": {
                    "source_digest": DIGEST,
                    "checked_at": consumer_applied_at,
                    "full_applied_at": consumer_applied_at,
                },
            },
        },
    )


def _drive(
    controller: ModuleType,
    *,
    arc_producer: bool = True,
    activation_status: int = fw.APPLY_REQUIRED_STATUS,
    apply_fails: bool = False,
    passes: int = 1,
) -> tuple[list[int], _Fleet, dict[str, Any]]:
    """Run consecutive passes over one fleet, returning each verdict."""
    data = _data(arc_producer=arc_producer)
    arc_host = "producer" if arc_producer else "consumer"
    statuses: list[int] = []
    fleet = _Fleet(
        controller,
        data,
        arc_host=arc_host,
        activation_status=activation_status,
        apply_fails=apply_fails,
    )
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-activation-") as raw:
        state_dir = Path(raw)
        _seed(controller, state_dir, consumer_due=not arc_producer)
        for index in range(passes):
            fleet.calls = []
            with (
                contextlib.redirect_stdout(io.StringIO()),
                contextlib.redirect_stderr(io.StringIO()),
            ):
                statuses.append(
                    controller.reconcile(
                        data,
                        _options(controller, state_dir, FIRST_PASS + index * PASS_STRIDE),
                        fleet.run,
                        _no_wait,
                    )
                )
        stored = controller.load_state(state_dir / controller.STATE_FILE)
    return statuses, fleet, stored


def _apply_required_opens_arc_capacity(controller: ModuleType, failures: list[str]) -> None:
    """The one held change the producer expects is a clean read, not a failed check."""
    statuses, fleet, stored = _drive(controller)
    verbs = fleet.verbs("producer")
    if "restore" not in verbs:
        failures.append(
            "the ARC activation check reported its one outstanding change as "
            f"apply-required and the sole capacity opener was never reached: {verbs}"
        )
    if "quarantine" in verbs:
        failures.append(
            "an ARC producer was drained for reporting the one held change the "
            f"controller expects of it: {verbs}"
        )
    if fleet.live_scale != DECLARED_RUNNERS:
        failures.append(
            f"the ARC host was left at a live ceiling of {fleet.live_scale} while "
            f"declared for {DECLARED_RUNNERS}"
        )
    if stored.get("stranded"):
        failures.append(f"a converged ARC producer was recorded at zero: {stored['stranded']}")
    receipt = stored["hosts"].get("producer") or {}
    if receipt.get("full_applied_at") != FIRST_PASS:
        failures.append(
            f"the opened ARC producer published no fresh full-verification receipt: {receipt}"
        )
    if statuses != [0]:
        failures.append(f"a pass that converged the whole fleet exited {statuses[0]}")


def _repeated_passes_never_strand_the_producer(controller: ModuleType, failures: list[str]) -> None:
    """Nothing about this shape recurs, so no record and no release may build up."""
    statuses, _fleet, stored = _drive(controller, passes=REPEATED_PASSES)
    if any(statuses):
        failures.append(f"repeated passes over a serving ARC producer exited {statuses}")
    if stored.get("stranded"):
        failures.append(
            "an ARC producer serving its declared runners accumulated a "
            f"stranded-at-zero record across {REPEATED_PASSES} passes: {stored['stranded']}"
        )
    consumer = stored["hosts"].get("consumer") or {}
    if controller.RELEASED_RECEIPT_KEY in consumer:
        failures.append(
            "the consumer was released onto a frozen image by a producer that never "
            f"left service: {consumer}"
        )


def _a_genuine_activation_failure_holds_at_zero(
    controller: ModuleType, failures: list[str]
) -> None:
    """Only apply-required is a clean read; any other status still fails closed."""
    statuses, fleet, stored = _drive(controller, activation_status=GENUINE_CHECK_FAILURE)
    verbs = fleet.verbs("producer")
    if "quarantine" not in verbs:
        failures.append(
            f"an activation check that genuinely failed left the host in service: {verbs}"
        )
    if "restore" in verbs:
        failures.append(
            f"capacity was opened over an ARC authority that never proved itself: {verbs}"
        )
    if fleet.live_scale:
        failures.append("a host whose ARC authority never proved itself was left serving")
    if (stored.get("stranded") or {}).get("producer", {}).get("passes") != 1:
        failures.append(
            f"a genuine activation failure stopped being recorded at zero: {stored.get('stranded')}"
        )
    if statuses != [ORDINARY_FAILURE_STATUS]:
        failures.append(f"a single failed activation exited {statuses[0]}")


def _an_unexpected_change_still_holds_at_zero(controller: ModuleType, failures: list[str]) -> None:
    """The status is counted, not waved through: a zero expectation still fails."""
    statuses, fleet, stored = _drive(controller, arc_producer=False)
    verbs = fleet.verbs("consumer")
    if "quarantine" not in verbs:
        failures.append(
            "an ARC host whose held check expects no outstanding change was opened "
            f"on one anyway: {verbs}"
        )
    if "restore" in verbs:
        failures.append(f"capacity was opened over an unexpected outstanding change: {verbs}")
    if (stored.get("stranded") or {}).get("consumer", {}).get("passes") != 1:
        failures.append(f"the host held at zero over it was not recorded: {stored.get('stranded')}")
    if statuses != [ORDINARY_FAILURE_STATUS]:
        failures.append(f"a held ARC consumer failing its activation check exited {statuses[0]}")


def _recovery_reopens_through_the_same_read(controller: ModuleType, failures: list[str]) -> None:
    """The recovery hook reopens through this activation sequence, so it reads it too."""
    statuses, fleet, stored = _drive(controller, apply_fails=True)
    verbs = fleet.verbs("producer")
    if "restore" not in verbs:
        failures.append(
            "recovery could not reopen last-known-good ARC capacity because the "
            f"activation check's apply-required status read as a failure: {verbs}"
        )
    if fleet.live_scale != DECLARED_RUNNERS:
        failures.append(
            "the pass reported the ARC host serving last-known-good capacity while "
            f"its live ceiling stayed at {fleet.live_scale}"
        )
    if stored.get("stranded"):
        failures.append(
            f"an ARC host proven back in service was still recorded at zero: {stored['stranded']}"
        )
    if statuses != [ORDINARY_FAILURE_STATUS]:
        failures.append(
            f"a recovered full verification exited {statuses[0]} instead of an ordinary "
            f"{ORDINARY_FAILURE_STATUS}"
        )


def _status_mapping_is_pinned(controller: ModuleType, failures: list[str]) -> None:
    """Pin every status the activation check can come back with."""
    data = _data()
    expected_changes = 3

    def answer(result: frp.CommandResult) -> tuple[bool, int]:
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return controller.inspect_activation_host(data, "producer", lambda _argv: result)

    apply_required = frp.CommandResult(fw.APPLY_REQUIRED_STATUS, "", "")
    if answer(apply_required) != (True, 1):
        failures.append(
            "an activation check reporting work outstanding was not read as a clean "
            f"check of one change: {answer(apply_required)}"
        )
    clean = _recap(controller, data, "producer", expected_changes)
    if answer(clean) != (True, expected_changes):
        failures.append(f"a clean activation recap lost its change count: {answer(clean)}")
    if answer(frp.CommandResult(GENUINE_CHECK_FAILURE, "", "")) != (False, 0):
        failures.append("a genuinely failed activation check was read as clean")
    if answer(frp.CommandResult(0, "nonsense\n", "")) != (False, 0):
        failures.append("a malformed activation recap was read as clean")


def run(controller: ModuleType) -> list[str]:
    """Return every failure about how the ARC activation check's status is read."""
    failures: list[str] = []
    _apply_required_opens_arc_capacity(controller, failures)
    _repeated_passes_never_strand_the_producer(controller, failures)
    _a_genuine_activation_failure_holds_at_zero(controller, failures)
    _an_unexpected_change_still_holds_at_zero(controller, failures)
    _recovery_reopens_through_the_same_read(controller, failures)
    _status_mapping_is_pinned(controller, failures)
    return failures
