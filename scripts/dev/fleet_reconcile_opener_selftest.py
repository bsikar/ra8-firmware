# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Regression selftests for the park release issuing the wrong opener (#888).

A host this controller left holding a DURABLE maintenance park is pinned at
zero admission: ``park_maintenance`` writes the marker before the drain that
was then refused, ``cmd_window`` refuses to raise admission while it is there,
and ``capacity-restore`` is the only thing that removes it.  The pass therefore
issues that restore for a parked host whose declaration verified this pass, and
clears the record when it lands.

It issued a BARE restore for every class.  That is the whole opener for a
Docker host and only the last step of one for an ARC scale set:
``reopen_capacity`` routes through ``_activate_arc`` precisely because ARC
capacity is opened by declaring its authority, proving that declaration while
admission is held at zero, and only then restoring.  A parked apply tears that
declaration down, so a restore issued on its own returns cleanly while the live
ceiling stays at ZERO.  The release then read that clean return as proof and
CLEARED the park record, and once the record is gone nothing escalates the host
(``parked_escalations``), nothing counts its capacity as forfeit
(``serving_hosts``), and the next pass budgets it as though it were serving.
An ARC scale set declared for all its runners with none of them in service, on
a pass that exits 0 reporting a converged fleet, is issue #888's own headline
arriving through the newest record.

These tests pin the release to the opener the host's class actually has:

* a parked ARC host is re-declared and proven at zero before the restore, and
  only then is its record cleared;
* an ARC authority that will not re-declare issues NO restore, keeps the
  record and escalates instead of reporting the host reopened;
* so does an activation check that does not verify;
* a parked Docker host is reopened exactly as before, by the bare restore;
* the release never drains a parked host on the way out;
* the opener decision and its held-check expectation, per class.
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
MARKER_ABSENT = "fleet-capacity: cannot restore without a durable maintenance marker\n"

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


def _data(producer_class: str = "arc_k8s") -> dict[str, Any]:
    """Return one image producer of the given class ahead of a Docker consumer."""
    return {
        "runner_image": {"source_host": "producer"},
        "hosts": {
            "producer": {
                "class": producer_class,
                "runners": {"instances": 1},
                "provisions": ["one", "two"],
            },
            "consumer-a": {
                "class": "docker_linux",
                "runners": {"instances": 1},
                "provisions": ["one"],
            },
        },
    }


def _recap(controller: ModuleType, data: dict[str, Any], host: str, changed: int) -> str:
    """Return the recap rows one converged host of this shape reports."""
    name = controller.recap_identity(data, host)
    row = f"{name} : ok=9 changed={{}} unreachable=0 failed=0 skipped=1 rescued=0 ignored=0\n"
    plays = len(data["hosts"][host]["provisions"])
    return row.format(changed) + row.format(0) * (plays - 1)


def _noise(controller: ModuleType, data: dict[str, Any], host: str) -> int:
    """Return the check noise one converged host of this kind reports."""
    producer = host == data["runner_image"]["source_host"]
    return controller.PRODUCER_CHECK_NOISE if producer else 0


class _Fleet:
    """One fake fleet that only serves ARC capacity while its authority is declared.

    ``capacity-restore`` patches the scale set's ceiling, so it reports the
    window target it converged to and puts runners in service only while the
    ARC declaration is in place.  A restore issued without that declaration is
    exactly the call that returns cleanly over a live ceiling of zero.
    """

    def __init__(  # noqa: PLR0913  # one fleet plus each fault a release has to meet
        self,
        controller: ModuleType,
        data: dict[str, Any],
        *,
        declared: bool = False,
        refuse_activate: bool = False,
        unverified_activation: bool = False,
        marker_absent: bool = False,
    ) -> None:
        self.controller = controller
        self.data = data
        self.issued: list[tuple[str, str]] = []
        self.declared = declared
        self.serving: dict[str, int] = {}
        self.refuse_activate = refuse_activate
        self.unverified_activation = unverified_activation
        self.marker_absent = marker_absent

    def _arc(self, host: str) -> bool:
        """Report whether this host's capacity is an ARC scale set."""
        return self.controller.capacity_opener(self.data, host) == "k8s"

    def run(self, argv: Sequence[str]) -> frp.CommandResult:
        """Answer one fleet command."""
        verb, host = _identity(argv)
        self.issued.append((verb, host))
        if verb == "activate":
            if self.refuse_activate:
                return frp.CommandResult(1, "", f"{host}: helm authority refused\n")
            self.declared = True
            return frp.CommandResult(0, "", "")
        if verb == "activation-check":
            if self.unverified_activation:
                return frp.CommandResult(2, "", f"{host}: activation check failed\n")
            return frp.CommandResult(
                0, _recap(self.controller, self.data, host, self.controller.PRODUCER_HELD_CHECK_NOISE), ""
            )
        if verb in {"check", "parked-check"}:
            changed = _noise(self.controller, self.data, host)
            return frp.CommandResult(0, _recap(self.controller, self.data, host, changed), "")
        if verb == "restore":
            if self.marker_absent:
                return frp.CommandResult(1, "", MARKER_ABSENT)
            declared = self.declared or not self._arc(host)
            target = self.data["hosts"][host]["runners"]["instances"] if declared else 0
            self.serving[host] = target
            return frp.CommandResult(
                0, f"fleet-capacity: restoring current window target {target}\n", ""
            )
        return frp.CommandResult(0, "", "")

    def verbs(self, host: str) -> list[str]:
        """Return every verb this pass issued against one host."""
        return [verb for verb, target in self.issued if target == host]


def _options(controller: ModuleType, state_dir: Path) -> object:
    """Return deterministic policy for one pass.

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
        now=NOW,
    )


def _no_wait(_seconds: float) -> None:
    """Take retry pacing out of these cases without skipping it."""


def _receipt() -> dict[str, Any]:
    """Return a receipt fresh enough that no full convergence is due."""
    return {"checked_at": NOW - 10, "full_applied_at": NOW - 10, "source_digest": DIGEST}


def _seed(controller: ModuleType, state_dir: Path, parked: Sequence[str]) -> None:
    """Write one converged fleet carrying durable parks on the named hosts."""
    (state_dir / controller.STATE_FILE).write_text(
        json.dumps(
            {
                "version": 1,
                "hosts": {host: _receipt() for host in ("producer", "consumer-a")},
                "stranded": {},
                "parked": {host: {"since": NOW - 4000, "passes": 2} for host in parked},
            }
        ),
        encoding="ascii",
    )


def _document(controller: ModuleType, state_dir: Path) -> dict[str, Any]:
    """Return the state file as it stands on disk."""
    path = state_dir / controller.STATE_FILE
    return json.loads(path.read_text(encoding="ascii")) if path.exists() else {}


def _run_pass(
    controller: ModuleType, state_dir: Path, fleet: _Fleet
) -> tuple[int, dict[str, Any]]:
    """Run one pass over the given fleet and return its status and state."""
    status = controller.reconcile(
        fleet.data, _options(controller, state_dir), fleet.run, sleep=_no_wait
    )
    return status, _document(controller, state_dir)


def _case_arc_park_is_redeclared(controller: ModuleType, failures: list[str]) -> None:
    """A parked ARC host is re-declared and proven at zero before the restore."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-opener-") as raw:
        state_dir = Path(raw)
        _seed(controller, state_dir, ("producer",))
        fleet = _Fleet(controller, _data())
        status, document = _run_pass(controller, state_dir, fleet)
    expected = ["check", "activate", "activation-check", "restore"]
    if fleet.verbs("producer") != expected:
        failures.append(
            f"opener: the park release issued {fleet.verbs('producer')}, expected {expected}; "
            "a bare restore returns cleanly while the ARC ceiling stays at zero"
        )
    if fleet.serving.get("producer") != 1:
        failures.append(
            f"opener: the released ARC host serves {fleet.serving.get('producer')} runners"
        )
    if document.get("parked"):
        failures.append(f"opener: a reopened park kept its record: {document.get('parked')}")
    if status:
        failures.append(f"opener: a pass that reopened its parked host exited {status}")


def _case_refused_declaration_keeps_record(controller: ModuleType, failures: list[str]) -> None:
    """An ARC authority that will not re-declare issues no restore and stays recorded."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-opener-") as raw:
        state_dir = Path(raw)
        _seed(controller, state_dir, ("producer",))
        fleet = _Fleet(controller, _data(), refuse_activate=True)
        status, document = _run_pass(controller, state_dir, fleet)
    if "restore" in fleet.verbs("producer"):
        failures.append(
            "opener: a restore was issued over an ARC declaration that never verified, "
            "which returns cleanly with nothing in service"
        )
    if "producer" not in document.get("parked", {}):
        failures.append(
            "opener: the park record was cleared although nothing reopened the host; "
            "nothing escalates or forfeits a host once its record is gone"
        )
    if status != controller.STRANDED_STATUS:
        failures.append(
            f"opener: a park this pass could not lift exited {status}, expected STRANDED_STATUS"
        )


def _case_unverified_activation_keeps_record(controller: ModuleType, failures: list[str]) -> None:
    """An activation check that does not verify is not an opened host either."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-opener-") as raw:
        state_dir = Path(raw)
        _seed(controller, state_dir, ("producer",))
        fleet = _Fleet(controller, _data(), unverified_activation=True)
        status, document = _run_pass(controller, state_dir, fleet)
    if "restore" in fleet.verbs("producer"):
        failures.append("opener: a restore followed an ARC activation check that failed")
    if "producer" not in document.get("parked", {}):
        failures.append("opener: an unproven ARC declaration still cleared the park record")
    if status != controller.STRANDED_STATUS:
        failures.append(f"opener: an unverified ARC release exited {status}")


def _case_docker_park_is_unchanged(controller: ModuleType, failures: list[str]) -> None:
    """A parked Docker host is reopened by the bare restore exactly as before."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-opener-") as raw:
        state_dir = Path(raw)
        _seed(controller, state_dir, ("consumer-a",))
        fleet = _Fleet(controller, _data("docker_linux"))
        status, document = _run_pass(controller, state_dir, fleet)
    if fleet.verbs("consumer-a") != ["check", "restore"]:
        failures.append(
            f"opener: a parked Docker host was reopened with {fleet.verbs('consumer-a')}, "
            "expected the bare restore its class has"
        )
    if document.get("parked"):
        failures.append("opener: a reopened Docker park kept its record")
    if status:
        failures.append(f"opener: a Docker park release exited {status}")


def _case_release_never_drains(controller: ModuleType, failures: list[str]) -> None:
    """A park is not drained on the way out, however the release ends."""
    for name, fleet_kwargs in (
        ("refused declaration", {"refuse_activate": True}),
        ("unverified declaration", {"unverified_activation": True}),
        ("marker already lifted", {"marker_absent": True}),
    ):
        with tempfile.TemporaryDirectory(prefix="ra8-fleet-opener-") as raw:
            state_dir = Path(raw)
            _seed(controller, state_dir, ("producer",))
            fleet = _Fleet(controller, _data(), **fleet_kwargs)  # type: ignore[arg-type]
            _status, document = _run_pass(controller, state_dir, fleet)
        if "quarantine" in fleet.verbs("producer"):
            failures.append(
                f"opener: the {name} path drained a host that already holds zero admission"
            )
        if name == "marker already lifted" and document.get("parked"):
            failures.append("opener: a park lifted outside this controller kept its record")


def _case_opener_decision(controller: ModuleType, failures: list[str]) -> None:
    """Pin the opener choice and its held-check expectation, per class."""
    arc = _data()
    docker = _data("docker_linux")
    openers = {
        "arc producer": controller.capacity_opener(arc, "producer"),
        "docker producer": controller.capacity_opener(docker, "producer"),
        "docker consumer": controller.capacity_opener(arc, "consumer-a"),
    }
    expected_openers = {"arc producer": "k8s", "docker producer": "docker", "docker consumer": "docker"}
    if openers != expected_openers:
        failures.append(f"opener: capacity_opener returned {openers}, expected {expected_openers}")
    changes = {
        "arc producer": controller.park_release_changes(arc, "producer"),
        "docker producer": controller.park_release_changes(docker, "producer"),
        "consumer": controller.park_release_changes(arc, "consumer-a"),
    }
    expected_changes = {
        "arc producer": controller.PRODUCER_HELD_CHECK_NOISE,
        "docker producer": controller.PRODUCER_CHECK_NOISE,
        "consumer": 0,
    }
    if changes != expected_changes:
        failures.append(
            f"opener: park_release_changes returned {changes}, expected {expected_changes}"
        )
    fleet = _Fleet(controller, arc, refuse_activate=True)
    if controller.open_parked_capacity(arc, "producer", fleet.run) is not None:
        failures.append("opener: a refused ARC declaration reported a restore result")
    docker_fleet = _Fleet(controller, docker)
    result = controller.open_parked_capacity(docker, "consumer-a", docker_fleet.run)
    if result is None or result.status or docker_fleet.verbs("consumer-a") != ["restore"]:
        failures.append(
            f"opener: the Docker arm issued {docker_fleet.verbs('consumer-a')} for one release"
        )


def run(controller: ModuleType) -> list[str]:
    """Run every park-release opener case against the controller under test."""
    failures: list[str] = []
    _case_arc_park_is_redeclared(controller, failures)
    _case_refused_declaration_keeps_record(controller, failures)
    _case_unverified_activation_keeps_record(controller, failures)
    _case_docker_park_is_unchanged(controller, failures)
    _case_release_never_drains(controller, failures)
    _case_opener_decision(controller, failures)
    return failures
