# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Regression selftests for a reopen that put no capacity back in service (#888).

``capacity-restore`` converges live admission to the host's CURRENT window
target and prints which one it used: the declared instance count on a host with
no quiet-hours block, and the quiet-hours count inside a declared window, which
``infra/fleet.yml`` sets to ZERO instances for ``win-ci``.  The controller read
none of that.  It took the restore's exit status as proof that capacity was
back, and on the docker arm confirmed it with an ordinary check, which reads the
DECLARATION: a live capacity change is temporary by construction, so the
declaration is converged either way and the check verifies whatever admission
the host actually holds.

A recovery reopen therefore claimed that a host drained by its own failed apply
was serving last-known-good capacity while the restore it had just issued had
converged that host to zero instances.  Nothing was recorded at zero, so
``stranded_escalations`` never fired however many passes it repeated for, the
dry run stayed green, the consumers earned ordinary receipts stamped
``full_applied_at`` and the pass exited 1 exactly like a one-off fault.  That is
issue #888's own dry-run evidence, a fleet still DECLARED for its runners while
it serves none of them, and the number that said so was already in the log.

These tests pin the reopen to the capacity it actually reopened:

* a reopen whose restore reported a zero window target records the host at zero
  and escalates once it has held it there for ``STRANDED_ESCALATION_PASSES``;
* a reopen whose restore reported real capacity still recovers, clears the
  record and keeps the pass an ordinary failure;
* a restore that reports no target at all claims nothing either way, so a host
  running an older copy of the capacity script behaves exactly as before;
* an ARC reopen refuses the same way, since both its apply and its recovery run
  through the one activation sequence;
* an ordinary apply is NOT failed by a zero window target, because converging
  the declaration is what that transaction claims and the zero is the
  operator's own policy rather than a failure to converge.
"""

from __future__ import annotations

import tempfile
from collections.abc import Sequence
from pathlib import Path
from types import ModuleType
from typing import Any

import fleet_reconcile_process as frp
import fleet_wsl as fw

APPLY_FAILURE_STATUS = 1
ORDINARY_FAILURE_STATUS = 1
FIRST_PASS = 2000
PASS_INTERVAL = 100
FULL_INTERVAL = 100
PRODUCER_INTERVAL = 50
DIGEST = "b" * 64
DECLARED_RUNNERS = 3
PRODUCER = "producer"
CONSUMER = "consumer"
ZERO_TARGET = "2026-09-17T23:05:11Z restoring current window target 0\n"
FULL_TARGET = f"2026-09-17T11:05:11Z restoring current window target {DECLARED_RUNNERS}\n"
SILENT_RESTORE = "2026-09-17T11:05:11Z restored\n"

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


def _data(*, arc_producer: bool = False) -> dict[str, Any]:
    """Return one image producer and one capacity-managed consumer."""
    return {
        "runner_image": {"source_host": PRODUCER},
        "hosts": {
            PRODUCER: {
                "class": "arc_k8s" if arc_producer else "docker_linux",
                "runners": {"instances": DECLARED_RUNNERS},
                "provisions": ["one", "two"],
            },
            CONSUMER: {
                "class": "docker_linux",
                "runners": {"instances": DECLARED_RUNNERS},
                "provisions": ["one"],
            },
        },
    }


def _clean(controller: ModuleType, data: dict[str, Any], host: str) -> frp.CommandResult:
    """Return the exact accepted check result for one host."""
    name = controller.recap_identity(data, host)
    producer = host == data["runner_image"]["source_host"]
    changed = controller.PRODUCER_CHECK_NOISE if producer else 0
    row = f"{name} : ok=9 changed={{}} unreachable=0 failed=0 skipped=1 rescued=0 ignored=0\n"
    plays = len(data["hosts"][host]["provisions"])
    return frp.CommandResult(0, row.format(changed) + row.format(0) * (plays - 1), "")


def _options(controller: ModuleType, state_dir: Path, now: int) -> object:
    """Return deterministic apply-mode policy for one pass at ``now``.

    The controller arrives as a module, so its option dataclass is handed
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


def _document(controller: ModuleType, state_dir: Path) -> dict[str, Any]:
    """Return the persisted state document, however the pass ended."""
    return controller.load_state(state_dir / controller.STATE_FILE)


def _record(controller: ModuleType, state_dir: Path) -> dict[str, Any]:
    """Return the persisted stranded-at-zero map, however the pass ended."""
    stored = _document(controller, state_dir).get("stranded")
    return stored if isinstance(stored, dict) else {}


def _recovery_pass(
    controller: ModuleType,
    state_dir: Path,
    now: int,
    *,
    restore: str,
    arc_producer: bool = False,
) -> tuple[int, list[tuple[str, str]]]:
    """Run one pass whose converged producer fails its apply and is reopened.

    The producer's check is CLEAN, so the apply is the periodic full
    verification and the failure carries no prior drift: that is the shape the
    recovery hook exists for, and the reopen it performs is what these cases
    are about.
    """
    data = _data(arc_producer=arc_producer)
    calls: list[tuple[str, str]] = []

    def fake_run(argv: Sequence[str]) -> frp.CommandResult:
        verb, host = _identity(argv)
        calls.append((verb, host))
        if verb in {"check", "parked-check"}:
            return _clean(controller, data, host)
        if verb == "activation-check":
            return frp.CommandResult(fw.APPLY_REQUIRED_STATUS, "", "")
        if verb == "parked-apply" and host == PRODUCER:
            return frp.CommandResult(APPLY_FAILURE_STATUS, "", "runner image build failed\n")
        if verb == "restore":
            return frp.CommandResult(0, restore, "")
        return frp.CommandResult(0, "", "")

    status = controller.reconcile(data, _options(controller, state_dir, now), fake_run, _no_wait)
    return status, calls


def _healthy_pass(
    controller: ModuleType, state_dir: Path, now: int, *, restore: str
) -> tuple[int, list[tuple[str, str]]]:
    """Run one pass in which every host converges; only the restore target varies."""
    data = _data()
    calls: list[tuple[str, str]] = []

    def fake_run(argv: Sequence[str]) -> frp.CommandResult:
        verb, host = _identity(argv)
        calls.append((verb, host))
        if verb in {"check", "parked-check"}:
            return _clean(controller, data, host)
        if verb == "restore":
            return frp.CommandResult(0, restore, "")
        return frp.CommandResult(0, "", "")

    status = controller.reconcile(data, _options(controller, state_dir, now), fake_run, _no_wait)
    return status, calls


def _zero_window_reopen_is_recorded(controller: ModuleType, failures: list[str]) -> None:
    """A reopen to a zero window target leaves the host recorded at zero, loudly."""
    total = controller.STRANDED_ESCALATION_PASSES
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-admission-") as raw:
        state_dir = Path(raw)
        statuses = [
            _recovery_pass(
                controller, state_dir, FIRST_PASS + index * PASS_INTERVAL, restore=ZERO_TARGET
            )[0]
            for index in range(total)
        ]
        record = _record(controller, state_dir)
    entry = record.get(PRODUCER, {})
    if entry.get("passes") != total or entry.get("since") != FIRST_PASS:
        failures.append(
            f"a host reopened to a ZERO window target was not recorded at zero capacity: {record}"
        )
    if statuses[-1] != controller.STRANDED_STATUS:
        failures.append(f"a fleet held at zero by its own reopens never escalated ({statuses})")


def _real_capacity_still_recovers(controller: ModuleType, failures: list[str]) -> None:
    """A reopen that put real capacity back is still a recovery, and still fails the pass."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-admission-") as raw:
        state_dir = Path(raw)
        status, calls = _recovery_pass(controller, state_dir, FIRST_PASS, restore=FULL_TARGET)
        record = _record(controller, state_dir)
        receipt = _document(controller, state_dir)["hosts"].get(CONSUMER)
    if record:
        failures.append(f"verified last-known-good capacity was recorded at zero: {record}")
    if status != ORDINARY_FAILURE_STATUS:
        failures.append(f"a recovered host stopped earning an ordinary failure ({status})")
    if ("parked-apply", CONSUMER) not in calls:
        failures.append("a producer serving last-known-good capacity held its consumers back")
    if not isinstance(receipt, dict) or controller.RELEASED_RECEIPT_KEY in receipt:
        failures.append(f"a serving producer made its consumer's receipt provisional: {receipt}")


def _silent_restore_claims_nothing(controller: ModuleType, failures: list[str]) -> None:
    """A restore that reports no target at all leaves the earlier evidence alone."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-admission-") as raw:
        state_dir = Path(raw)
        status, calls = _recovery_pass(controller, state_dir, FIRST_PASS, restore=SILENT_RESTORE)
        record = _record(controller, state_dir)
    if record:
        failures.append(
            f"a restore that said nothing about admission was read as zero capacity: {record}"
        )
    if status != ORDINARY_FAILURE_STATUS:
        failures.append(f"a silent restore changed the pass verdict ({status})")
    if ("check", PRODUCER) not in calls[1:]:
        failures.append(f"the reopen skipped its verifying check: {calls}")


def _arc_reopen_refuses_zero_admission(controller: ModuleType, failures: list[str]) -> None:
    """The ARC arm refuses a zero window target too: both its paths share one opener."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-admission-") as raw:
        state_dir = Path(raw)
        status, calls = _recovery_pass(
            controller, state_dir, FIRST_PASS, restore=ZERO_TARGET, arc_producer=True
        )
        record = _record(controller, state_dir)
    if record.get(PRODUCER, {}).get("passes") != 1:
        failures.append(f"an ARC scale set reopened to a zero ceiling was not recorded: {record}")
    if status != ORDINARY_FAILURE_STATUS:
        failures.append(f"the ARC reopen refusal changed the pass verdict ({status})")
    if ("activate", PRODUCER) not in calls:
        failures.append(f"the ARC reopen never declared its authority: {calls}")


def _apply_is_not_failed_by_policy(controller: ModuleType, failures: list[str]) -> None:
    """Converging the declaration is what an apply claims; a zero window target is policy."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-admission-") as raw:
        state_dir = Path(raw)
        status, _calls = _healthy_pass(controller, state_dir, FIRST_PASS, restore=ZERO_TARGET)
        record = _record(controller, state_dir)
        receipts = _document(controller, state_dir)["hosts"]
    if status:
        failures.append(f"a converged pass was failed by a host's own window policy ({status})")
    if record:
        failures.append(f"a declared quiet-hours target was recorded as stranding: {record}")
    if sorted(receipts) != [CONSUMER, PRODUCER]:
        failures.append(f"a converged pass stopped publishing its receipts: {sorted(receipts)}")


def _policy_stays_narrow(controller: ModuleType, failures: list[str]) -> None:
    """Unit-level: the reader answers to the restore's own reported target."""
    if controller.restored_admission(ZERO_TARGET) != 0:
        failures.append("a zero window target was not read off the restore")
    if controller.restored_admission(FULL_TARGET) != DECLARED_RUNNERS:
        failures.append("a real window target was not read off the restore")
    if controller.restored_admission(SILENT_RESTORE) is not None:
        failures.append("a restore that reported no target was read as one anyway")
    if controller.restored_admission(FULL_TARGET + ZERO_TARGET) != 0:
        failures.append("the target a restore ended on was not the one that counted")
    zero = frp.CommandResult(0, ZERO_TARGET, "")
    if controller.restore_admission(PRODUCER, zero) != 0:
        failures.append("a restore that put nothing in service was reported as capacity")
    full = frp.CommandResult(0, FULL_TARGET, "")
    if controller.restore_admission(PRODUCER, full) != DECLARED_RUNNERS:
        failures.append("a restore that put real capacity back was not reported as such")
    data = _data(arc_producer=True)
    calls: list[tuple[str, str]] = []

    def arc_run(argv: Sequence[str]) -> frp.CommandResult:
        verb, host = _identity(argv)
        calls.append((verb, host))
        if verb == "activation-check":
            return frp.CommandResult(fw.APPLY_REQUIRED_STATUS, "", "")
        if verb == "restore":
            return frp.CommandResult(0, ZERO_TARGET, "")
        return frp.CommandResult(0, "", "")

    opened, _changed = controller._activate_arc(data, PRODUCER, arc_run, 1)  # noqa: SLF001
    if not opened:
        failures.append("an ordinary ARC apply was failed by a declared zero window target")
    if any(verb == "quarantine" for verb, _host in calls):
        failures.append(f"a host already at zero admission was drained again: {calls}")


def run(controller: ModuleType) -> list[str]:
    """Return every reopen-without-capacity failure."""
    failures: list[str] = []
    _zero_window_reopen_is_recorded(controller, failures)
    _real_capacity_still_recovers(controller, failures)
    _silent_restore_claims_nothing(controller, failures)
    _arc_reopen_refuses_zero_admission(controller, failures)
    _apply_is_not_failed_by_policy(controller, failures)
    _policy_stays_narrow(controller, failures)
    return failures
