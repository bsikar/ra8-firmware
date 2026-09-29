# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Selftests for reopening last-known-good capacity through each class's real opener.

Recovery reopened every drained host with a bare ``capacity-restore``.  That is
the whole opener for a Docker host and only the last step of one for an ARC
scale set, whose capacity is opened by declaring its authority, proving the
declaration while admission is still held at zero, and only then restoring.  A
parked apply tears that declaration down, so the lone restore returned cleanly
over a live ceiling still at ZERO and the ordinary check that followed it read
the DECLARATION rather than the live scale.  The controller reported the host
as serving last-known-good capacity, wrote no stranded-at-zero record, cleared
any record it already held, and failed the pass like a one-off fault: issue
#888's ARC still declared for all its runners with none of them in service, and
no escalation however long it stays that way.
"""

from __future__ import annotations

import tempfile
from collections.abc import Sequence
from pathlib import Path
from types import ModuleType
from typing import Any

import fleet_reconcile_process as frp

DIGEST = "b" * 64
NOW = 6000
STALE_APPLIED_AT = 100
FRESH_APPLIED_AT = 5990
FULL_INTERVAL = 100
PRODUCER_INTERVAL = 50
DECLARED_RUNNERS = 6
DRIFTED_TASKS = 4
HELD_RECORD = {"since": 2000, "passes": 2}


def _data(producer_class: str) -> dict[str, Any]:
    """Return one producer of the given class plus one Docker consumer."""
    return {
        "runner_image": {"source_host": "producer"},
        "hosts": {
            "producer": {
                "class": producer_class,
                "runners": {"instances": DECLARED_RUNNERS},
                "provisions": ["one", "two"],
            },
            "consumer": {
                "class": "docker_linux",
                "runners": {"instances": 1},
                "provisions": ["one"],
            },
        },
    }


def _identity(argv: Sequence[str]) -> tuple[str, str]:
    """Return the reconciler verb and host from fleet argv."""
    names = {
        "reconcile-parked-apply": "parked-apply",
        "reconcile-parked-check": "parked-check",
        "reconcile-activate": "activate",
        "reconcile-activation-check": "activation-check",
        "capacity-quarantine": "quarantine",
        "capacity-restore": "restore",
    }
    return names.get(argv[2], argv[2]), argv[-1]


def _check(
    controller: ModuleType, data: dict[str, Any], host: str, changed: int
) -> frp.CommandResult:
    """Return one successful check whose first play reports ``changed``."""
    name = controller.recap_identity(data, host)
    row = f"{name} : ok=9 changed={{}} unreachable=0 failed=0 skipped=1 rescued=0 ignored=0\n"
    plays = len(data["hosts"][host]["provisions"])
    return frp.CommandResult(0, row.format(changed) + row.format(0) * (plays - 1), "")


def _clean(controller: ModuleType, data: dict[str, Any], host: str) -> frp.CommandResult:
    """Return the exact accepted check evidence for one converged host."""
    noise = controller.PRODUCER_CHECK_NOISE if host == "producer" else 0
    return _check(controller, data, host, noise)


def _held(controller: ModuleType, data: dict[str, Any]) -> frp.CommandResult:
    """Return the accepted ARC evidence for a producer checked at held zero."""
    return _check(controller, data, "producer", controller.PRODUCER_HELD_CHECK_NOISE)


def _no_wait(_seconds: float) -> None:
    """Take retry pacing out of these cases without skipping it."""


class _Fleet:
    """One producer modelled by what its live ceiling actually does.

    On an ARC scale set ``restore`` is the sole opener and it only lifts the
    ceiling while the ARC authority is declared, which is exactly why
    ``activate`` and its held-zero check come first; a parked apply tears that
    declaration down.  A Docker host has no such authority, so its restore
    opens capacity on its own.
    """

    def __init__(self, controller: ModuleType, data: dict[str, Any], **refusals: int) -> None:
        """Start the fleet serving its declared runners."""
        self.controller = controller
        self.data = data
        self.refusals = refusals
        self.calls: list[tuple[str, str]] = []
        self.live_scale = DECLARED_RUNNERS
        self.arc = data["hosts"]["producer"]["class"] == "arc_k8s"
        self.declared = True

    def run(self, argv: Sequence[str]) -> frp.CommandResult:
        """Answer one fleet command, moving the modelled capacity with it."""
        verb, host = _identity(argv)
        self.calls.append((verb, host))
        status = self.refusals.get(verb, 0)
        if host == "producer":
            if verb == "parked-apply":
                self.live_scale = 0
                self.declared = not self.arc
                return frp.CommandResult(1, "", "curl: (28) operation timed out\n")
            if verb == "quarantine":
                self.live_scale = 0
            if verb == "activate" and not status:
                self.declared = True
            if verb == "restore" and not status and self.declared:
                self.live_scale = DECLARED_RUNNERS
        if status:
            return frp.CommandResult(status, "", "")
        if verb == "activation-check":
            return _held(self.controller, self.data)
        if verb in {"check", "parked-check"}:
            return _clean(self.controller, self.data, host)
        return frp.CommandResult(0, "", "")

    def verbs(self, host: str = "producer") -> list[str]:
        """Return the verbs issued against one host, in order."""
        return [verb for verb, target in self.calls if target == host]


def _options(controller: ModuleType, state_dir: Path) -> object:
    """Return apply-mode options whose producer is due a full verification."""
    return controller.ReconcileOptions(
        mode="apply",
        force=False,
        source_digest=DIGEST,
        state_dir=state_dir,
        full_interval=FULL_INTERVAL,
        producer_interval=PRODUCER_INTERVAL,
        now=NOW,
    )


def _drive(
    controller: ModuleType, producer_class: str, *, stranded: bool = False, **refusals: int
) -> tuple[int, _Fleet, dict[str, Any]]:
    """Run one pass whose producer's parked applies all fail after a clean check."""
    data = _data(producer_class)
    fleet = _Fleet(controller, data, **refusals)
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-reactivate-") as raw:
        state_dir = Path(raw)
        document: dict[str, Any] = {
            "version": 1,
            "hosts": {
                "producer": {"source_digest": DIGEST, "full_applied_at": STALE_APPLIED_AT},
                "consumer": {"source_digest": DIGEST, "full_applied_at": FRESH_APPLIED_AT},
            },
        }
        if stranded:
            document["stranded"] = {"producer": dict(HELD_RECORD)}
        controller.save_state(state_dir / controller.STATE_FILE, document)
        status = controller.reconcile(data, _options(controller, state_dir), fleet.run, _no_wait)
        stored = controller.load_state(state_dir / controller.STATE_FILE)
    return status, fleet, stored


def _arc_reopen_uses_its_own_opener(controller: ModuleType, failures: list[str]) -> None:
    """An ARC host reopened for recovery is declared, proved at zero, then restored."""
    status, fleet, stored = _drive(controller, "arc_k8s")
    verbs = fleet.verbs()
    if "activate" not in verbs or "activation-check" not in verbs:
        failures.append(
            "ARC recovery reopened last-known-good capacity with a bare restore, "
            f"never declaring or proving its authority: {verbs}"
        )
    elif verbs.index("activate") > verbs.index("restore"):
        failures.append("ARC recovery restored before declaring the authority that opens it")
    if fleet.live_scale != DECLARED_RUNNERS:
        failures.append(
            "the pass reported the ARC host serving last-known-good capacity while its "
            f"live ceiling stayed at {fleet.live_scale}"
        )
    if stored.get("stranded"):
        failures.append("an ARC host proven back in service was still recorded at zero")
    if status != 1:
        failures.append(f"a recovered full verification exited {status} instead of an ordinary 1")


def _arc_reopen_is_verified_while_admission_is_held(
    controller: ModuleType, failures: list[str]
) -> None:
    """The ARC authority is proved before the ceiling moves, not after."""
    _status, fleet, _stored = _drive(controller, "arc_k8s")
    verbs = fleet.verbs()
    tail = verbs[-3:]
    if tail != ["activate", "activation-check", "restore"]:
        failures.append(f"ARC recovery's reopen order drifted: {tail}")
    if verbs.count("quarantine") != 1:
        failures.append(
            f"a recovered ARC host was drained {verbs.count('quarantine')} time(s) instead of "
            "once for its exhausted mutation"
        )


def _undeclarable_arc_host_stays_at_zero(controller: ModuleType, failures: list[str]) -> None:
    """A reopen that cannot declare ARC authority holds the host at zero and says so."""
    status, fleet, stored = _drive(controller, "arc_k8s", activate=1)
    verbs = fleet.verbs()
    if "restore" in verbs:
        failures.append("ARC capacity was restored after its authority failed to declare")
    if verbs[-1] != "quarantine":
        failures.append(f"an ARC host that could not be declared was left open: {verbs}")
    if fleet.live_scale:
        failures.append("an unverified ARC reopen left live capacity behind")
    record = stored.get("stranded", {}).get("producer")
    if not record:
        failures.append("an ARC host held at zero by a failed reopen earned no record")
    elif record["passes"] != 1:
        failures.append(
            f"the held ARC host recorded {record['passes']} passes instead of its first"
        )
    if "producer" in stored["hosts"]:
        failures.append("a host held at zero kept its receipt")
    if status != 1:
        failures.append(f"a pass holding one host at zero exited {status} instead of 1")


def _unproven_arc_authority_stays_at_zero(controller: ModuleType, failures: list[str]) -> None:
    """A declaration that does not verify at held zero never reaches the opener."""
    status, fleet, stored = _drive(controller, "arc_k8s", **{"activation-check": 1})
    verbs = fleet.verbs()
    if "restore" in verbs:
        failures.append("ARC capacity was restored over an authority that did not verify")
    if verbs[-2:] != ["activation-check", "quarantine"]:
        failures.append(f"an unverified ARC declaration did not retain zero: {verbs}")
    if not stored.get("stranded", {}).get("producer"):
        failures.append("an unverified ARC reopen recorded no zero capacity")
    if status != 1:
        failures.append(f"an unverified ARC reopen exited {status} instead of 1")


def _a_held_record_survives_a_reopen_that_never_landed(
    controller: ModuleType, failures: list[str]
) -> None:
    """A failed ARC reopen may not clear a record earned by earlier passes."""
    _status, _fleet, stored = _drive(controller, "arc_k8s", stranded=True, activate=1)
    record = stored.get("stranded", {}).get("producer")
    if not record:
        failures.append("a failed ARC reopen cleared the record of a host still at zero")
    elif record["since"] != HELD_RECORD["since"] or record["passes"] != HELD_RECORD["passes"] + 1:
        failures.append(f"the held ARC host's record was rewritten rather than aged: {record}")


def _docker_recovery_is_unchanged(controller: ModuleType, failures: list[str]) -> None:
    """A Docker host's opener is the restore it always was."""
    status, fleet, stored = _drive(controller, "docker_linux")
    verbs = fleet.verbs()
    if "activate" in verbs or "activation-check" in verbs:
        failures.append(f"a Docker host was reopened through the ARC opener: {verbs}")
    if verbs[-2:] != ["restore", "check"]:
        failures.append(f"Docker recovery's reopen order drifted: {verbs}")
    if fleet.live_scale != DECLARED_RUNNERS:
        failures.append("a recovered Docker host was left without capacity")
    if stored.get("stranded"):
        failures.append("a recovered Docker host was recorded at zero")
    if status != 1:
        failures.append(f"a recovered Docker full verification exited {status} instead of 1")


def _opener_policy_is_per_class(controller: ModuleType, failures: list[str]) -> None:
    """Unit level: the opener follows the host's class, and proves capacity either way."""
    for producer_class, expected in (("arc_k8s", "k8s"), ("docker_linux", "docker")):
        data = _data(producer_class)
        if controller.capacity_opener(data, "producer") != expected:
            failures.append(f"{producer_class} reported the wrong capacity opener")
        fleet = _Fleet(controller, data)
        fleet.live_scale = 0
        fleet.declared = not fleet.arc
        opened = controller.reopen_capacity(
            data,
            "producer",
            fleet.run,
            serving_changes=controller.PRODUCER_CHECK_NOISE,
            held_changes=controller.PRODUCER_HELD_CHECK_NOISE,
        )
        if not opened:
            failures.append(f"{producer_class} could not be reopened by its own opener")
        if fleet.live_scale != DECLARED_RUNNERS:
            failures.append(
                f"{producer_class} reported reopened while its live ceiling stayed at "
                f"{fleet.live_scale}"
            )
    data = _data("docker_linux")
    fleet = _Fleet(controller, data, restore=1)
    if controller.reopen_capacity(
        data,
        "producer",
        fleet.run,
        serving_changes=controller.PRODUCER_CHECK_NOISE,
        held_changes=controller.PRODUCER_HELD_CHECK_NOISE,
    ):
        failures.append("a refused restore was reported as reopened capacity")
    if fleet.verbs()[-1] != "quarantine":
        failures.append("a refused restore did not drain the host after it")


def run(controller: ModuleType) -> list[str]:
    """Return every capacity-reopen failure this controller still has."""
    failures: list[str] = []
    _arc_reopen_uses_its_own_opener(controller, failures)
    _arc_reopen_is_verified_while_admission_is_held(controller, failures)
    _undeclarable_arc_host_stays_at_zero(controller, failures)
    _unproven_arc_authority_stays_at_zero(controller, failures)
    _a_held_record_survives_a_reopen_that_never_landed(controller, failures)
    _docker_recovery_is_unchanged(controller, failures)
    _opener_policy_is_per_class(controller, failures)
    return failures
