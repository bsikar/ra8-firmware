# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Regression selftests for last-known-good capacity recovery (issue #888).

A transient producer mutation failure, such as a locked dependency download
that times out part way through the canonical image build, must not strand the
fleet at zero capacity.  These tests pin the three outcomes that separate
recovery from fail-closed draining:

* a converged host whose *periodic* apply fails is reopened at last-known-good
  capacity, the pass still reports failure, and consumers are not blocked;
* a host that was genuinely drifting stays drained and keeps blocking its
  consumers, exactly as before;
* a reopen that does not verify is driven back to zero and blocks again.
"""

from __future__ import annotations

import tempfile
from collections.abc import Sequence
from pathlib import Path
from types import ModuleType
from typing import Any

import fleet_reconcile_process as frp

DRIFTED_TASKS = 5
DRAINED_TWICE = 2
STALE_APPLIED_AT = 900
FRESH_APPLIED_AT = 975
NOW = 1000
FULL_INTERVAL = 100
PRODUCER_INTERVAL = 50
DIGEST = "a" * 64


def _data() -> dict[str, Any]:
    """Return one producer plus one consumer, both capacity-managed."""
    return {
        "runner_image": {"source_host": "producer"},
        "hosts": {
            "consumer": {
                "class": "docker_wsl",
                "runners": {"instances": 1},
                "provisions": ["one"],
            },
            "producer": {
                "class": "docker_linux",
                "runners": {"instances": 1},
                "provisions": ["one", "two"],
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


def _no_wait(_seconds: float) -> None:
    """Take retry pacing out of these recovery cases without skipping it."""


def _run_case(
    controller: ModuleType,
    state_dir: Path,
    *,
    producer_drift: int,
    restore_verifies: bool,
) -> tuple[int, list[tuple[str, str]], dict[str, Any]]:
    """Drive one reconciliation whose producer applies always fail."""
    data = _data()
    controller.save_state(
        state_dir / controller.STATE_FILE,
        {
            "version": 1,
            "hosts": {
                "producer": {"source_digest": DIGEST, "full_applied_at": STALE_APPLIED_AT},
                "consumer": {"source_digest": DIGEST, "full_applied_at": FRESH_APPLIED_AT},
            },
        },
    )
    calls: list[tuple[str, str]] = []
    restored = False

    def fake_run(argv: Sequence[str]) -> frp.CommandResult:
        nonlocal restored
        verb, host = _identity(argv)
        calls.append((verb, host))
        if verb == "restore" and host == "producer":
            restored = True
            return frp.CommandResult(0, "", "")
        if verb == "parked-apply" and host == "producer":
            return frp.CommandResult(1, "", "curl: (28) operation timed out\n")
        if verb in {"check", "parked-check"}:
            if host == "producer" and producer_drift and not restored:
                return _check(controller, data, host, producer_drift)
            if host == "producer" and restored and not restore_verifies:
                return _check(controller, data, host, DRIFTED_TASKS)
            return _clean(controller, data, host)
        return frp.CommandResult(0, "", "")

    options = controller.ReconcileOptions(
        mode="apply",
        force=False,
        source_digest=DIGEST,
        state_dir=state_dir,
        full_interval=FULL_INTERVAL,
        producer_interval=PRODUCER_INTERVAL,
        now=NOW,
    )
    status = controller.reconcile(data, options, fake_run, _no_wait)
    stored = controller.load_state(state_dir / controller.STATE_FILE)["hosts"]
    return status, calls, stored


def _periodic_failure_recovers(controller: ModuleType, failures: list[str]) -> None:
    """A converged producer must be reopened, not stranded, after a blip."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-recovery-") as raw:
        status, calls, stored = _run_case(
            controller, Path(raw), producer_drift=0, restore_verifies=True
        )
    if status != 1:
        failures.append("a failed periodic producer apply was reported as success")
    if ("quarantine", "producer") not in calls:
        failures.append("a failed producer apply skipped its fail-closed drain")
    elif ("restore", "producer") not in calls:
        failures.append("converged producer was stranded at zero after a transient apply failure")
    elif calls.index(("restore", "producer")) < calls.index(("quarantine", "producer")):
        failures.append("capacity was reopened before the failed apply was drained")
    if ("check", "consumer") not in calls:
        failures.append("recovered producer capacity still blocked every consumer")
    if "producer" in stored:
        failures.append("a failed producer apply kept its receipt and skipped the next retry")


def _drifting_failure_stays_drained(controller: ModuleType, failures: list[str]) -> None:
    """Real drift plus a failed apply must keep the old fail-closed outcome."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-recovery-") as raw:
        status, calls, _stored = _run_case(
            controller, Path(raw), producer_drift=DRIFTED_TASKS, restore_verifies=True
        )
    if status != 1:
        failures.append("a drifting producer that failed to apply was reported as success")
    if ("restore", "producer") in calls:
        failures.append("a drifting producer was reopened without a successful apply")
    if ("check", "consumer") in calls:
        failures.append("a drained drifting producer no longer blocked its consumers")


def _unverified_recovery_stays_drained(controller: ModuleType, failures: list[str]) -> None:
    """A reopen that does not verify must return the host to zero."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-recovery-") as raw:
        status, calls, _stored = _run_case(
            controller, Path(raw), producer_drift=0, restore_verifies=False
        )
    if status != 1:
        failures.append("an unverified capacity reopen was reported as success")
    if calls.count(("quarantine", "producer")) < DRAINED_TWICE:
        failures.append("an unverified capacity reopen was left serving work")
    if ("check", "consumer") in calls:
        failures.append("an unverified capacity reopen did not block its consumers")


def run(controller: ModuleType) -> list[str]:
    """Return every last-known-good recovery failure."""
    failures: list[str] = []
    _periodic_failure_recovers(controller, failures)
    _drifting_failure_stays_drained(controller, failures)
    _unverified_recovery_stays_drained(controller, failures)
    return failures
