# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Regression selftests for what a read-only dry run reports about zero capacity (#888).

The stranded-at-zero record is the only state that tells a fleet that is fine
from one this controller has already taken to zero, and it was read out by an
apply pass alone.  ``--mode check``, the dry run an operator reaches for to ask
what the fleet looks like, therefore reported the declaration and nothing else:
it never said that hosts were being held at zero, and it exited 0 while they
sat there.  That is the diagnostic in issue #888's own evidence, where a later
dry run showed ARC still declared for six and TrueNAS for one while both had
been drained for days.  Draining is an override the declaration knows nothing
about, so a parked host's read-only check comes back CURRENT and only the
record knows better.

These tests pin the report and its scope:

* a dry run over a host held at zero past the escalation threshold says so and
  earns ``STRANDED_STATUS``, while still writing nothing and mutating nothing;
* a dry run over a record below the threshold names the host without claiming
  the verdict it has not earned;
* a retired host's stale record keeps the dry run green, because pruning it is
  a write only an apply pass makes;
* apply mode is unchanged: it still escalates a managed host at zero and still
  prunes what it no longer manages;
* the scope and the report are pinned at unit level.
"""

from __future__ import annotations

import tempfile
from collections.abc import Sequence
from pathlib import Path
from types import ModuleType
from typing import Any

import fleet_reconcile_process as frp

CHECK_FAILURE_STATUS = 2
FIRST_PASS = 9000
DRAINED_AT = 5000
FULL_INTERVAL = 1000
PRODUCER_INTERVAL = 500
DIGEST = "c" * 64
PRODUCER = "image-a"
CONSUMER = "consumer"
RETIRED = "retired-runner"
RETIRED_PASSES = 9

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
    """Return the exact accepted check result for one host.

    A drained host is parked by an override the declaration says nothing about,
    so its read-only check reports the declaration satisfied: exactly the dry
    run in issue #888 that showed ARC still declared for six.
    """
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


def _seed(controller: ModuleType, state_dir: Path, document: dict[str, Any]) -> None:
    """Write one starting state file for a pass to read."""
    controller.save_state(state_dir / controller.STATE_FILE, document)


def _held(passes: int, host: str = CONSUMER) -> dict[str, Any]:
    """Return a state file whose only content is one host recorded at zero."""
    return {
        "version": 1,
        "hosts": {},
        "stranded": {host: {"since": DRAINED_AT, "passes": passes}},
    }


def _pass(
    controller: ModuleType,
    state_dir: Path,
    now: int,
    *,
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
            return _clean(controller, data, host)
        return frp.CommandResult(0, "", "")

    status = controller.reconcile(
        data, _options(controller, state_dir, now, mode), fake_run, _no_wait
    )
    return status, calls


def _dry_run_reports_a_host_held_at_zero(controller: ModuleType, failures: list[str]) -> None:
    """A read-only pass may not exit 0 over capacity this controller is holding at zero."""
    escalating = controller.STRANDED_ESCALATION_PASSES + 2
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-dryrun-") as raw:
        state_dir = Path(raw)
        _seed(controller, state_dir, _held(escalating))
        before = _state(controller, state_dir)
        status, calls = _pass(controller, state_dir, FIRST_PASS, mode="check")
        after = _state(controller, state_dir)
    if status != controller.STRANDED_STATUS:
        failures.append(
            f"a dry run over a host held at zero for {escalating} consecutive passes reported "
            f"{status}; the declaration matched, so the whole fleet read as converged while "
            "that capacity was out of service"
        )
    mutations = [call for call in calls if controller.capacity_mutation(call[0])]
    if mutations:
        failures.append(f"a dry run issued capacity mutations {mutations}")
    if after != before:
        failures.append("a dry run wrote to the state file it only had to report on")


def _dry_run_names_a_record_below_the_threshold(
    controller: ModuleType, failures: list[str]
) -> None:
    """A record too young to escalate is still worth saying out loud, and only that."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-dryrun-") as raw:
        state_dir = Path(raw)
        _seed(controller, state_dir, _held(1))
        status, _calls = _pass(controller, state_dir, FIRST_PASS, mode="check")
        after = _state(controller, state_dir)
    if status:
        failures.append(
            f"a dry run claimed {status} over a host recorded at zero for one pass; that record "
            "has not earned the escalation an apply pass would have to reach first"
        )
    if after.get("stranded", {}).get(CONSUMER, {}).get("passes") != 1:
        failures.append("a dry run aged or dropped the record it only had to report on")
    named = controller.report_recorded_zero(
        {
            CONSUMER: {"since": DRAINED_AT, "passes": 1},
            PRODUCER: {"since": DRAINED_AT, "passes": controller.STRANDED_ESCALATION_PASSES},
        },
        FIRST_PASS,
    )
    if named != [CONSUMER]:
        failures.append(
            f"the below-threshold report named {named}; it exists to cover exactly the hosts "
            "the escalation does not, so it must not double up on one it does"
        )


def _retired_record_keeps_the_dry_run_green(controller: ModuleType, failures: list[str]) -> None:
    """A check pass cannot prune, so it must not escalate what it cannot clean up."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-dryrun-") as raw:
        state_dir = Path(raw)
        _seed(controller, state_dir, _held(RETIRED_PASSES, host=RETIRED))
        status, _calls = _pass(controller, state_dir, FIRST_PASS, mode="check")
        after = _state(controller, state_dir)
    if status:
        failures.append(
            f"a dry run reported {status} over a retired host's stale record; a verdict that "
            "never goes back to zero is the noise the prune exists to stop, and a check pass "
            "cannot prune it"
        )
    if RETIRED not in after.get("stranded", {}):
        failures.append("a dry run pruned a record it does not persist")


def _apply_mode_is_unchanged(controller: ModuleType, failures: list[str]) -> None:
    """The apply pass still escalates what it manages and still prunes what it does not."""
    seeded = _held(controller.STRANDED_ESCALATION_PASSES - 1)
    seeded["stranded"][RETIRED] = {"since": DRAINED_AT, "passes": RETIRED_PASSES}
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-dryrun-") as raw:
        state_dir = Path(raw)
        _seed(controller, state_dir, seeded)
        status, _calls = _pass(controller, state_dir, FIRST_PASS, check_fails=CONSUMER)
        after = _state(controller, state_dir)
    if status != controller.STRANDED_STATUS:
        failures.append(
            f"an apply pass over a managed host that stayed at zero reported {status} instead "
            "of the stranding escalation"
        )
    if RETIRED in after.get("stranded", {}):
        failures.append("an apply pass stopped pruning the records it no longer manages")
    if after.get("stranded", {}).get(CONSUMER, {}).get("passes") != (
        controller.STRANDED_ESCALATION_PASSES
    ):
        failures.append(
            "an apply pass stopped counting the pass a host spent at zero behind a failed check"
        )


def _escalation_scope_is_the_declaration(controller: ModuleType, failures: list[str]) -> None:
    """Pin the scope both modes reason about, and that only the report differs."""
    stranding = {
        CONSUMER: {"since": DRAINED_AT, "passes": controller.STRANDED_ESCALATION_PASSES},
        RETIRED: {"since": DRAINED_AT, "passes": RETIRED_PASSES},
    }
    managed = controller.managed_stranding(stranding, [PRODUCER, CONSUMER])
    if set(managed) != {CONSUMER}:
        failures.append(f"the managed scope was {sorted(managed)} instead of the declaration's")
    if set(stranding) != {CONSUMER, RETIRED}:
        failures.append("scoping the record mutated it; only the prune may drop an entry")
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-dryrun-") as raw:
        state_dir = Path(raw)
        for mode in ("apply", "check"):
            escalated = controller.pass_escalations(
                dict(stranding),
                [PRODUCER, CONSUMER],
                _options(controller, state_dir, FIRST_PASS, mode),
            )
            if escalated != [CONSUMER]:
                failures.append(
                    f"a {mode} pass escalated {escalated}; both modes read the same record, so "
                    "they owe the same verdict"
                )
        if controller.pass_escalations(
            {}, [PRODUCER, CONSUMER], _options(controller, state_dir, FIRST_PASS, "check")
        ):
            failures.append("a dry run escalated a fleet with nothing recorded at zero")


def run(controller: ModuleType) -> list[str]:
    """Return every failure about what a read-only dry run reports about zero capacity."""
    failures: list[str] = []
    _dry_run_reports_a_host_held_at_zero(controller, failures)
    _dry_run_names_a_record_below_the_threshold(controller, failures)
    _retired_record_keeps_the_dry_run_green(controller, failures)
    _apply_mode_is_unchanged(controller, failures)
    _escalation_scope_is_the_declaration(controller, failures)
    return failures
