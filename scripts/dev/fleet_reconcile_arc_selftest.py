# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""ARC activation ordering and held-admission reconciliation selftests."""

from __future__ import annotations

from collections.abc import Callable, Sequence
from typing import Any, Protocol

import fleet_reconcile_process as frp


class ApplyHost(Protocol):
    """Apply and validate one host through the reconciler transaction."""

    def __call__(
        self,
        data: dict[str, Any],
        host: str,
        run: Callable[[Sequence[str]], frp.CommandResult],
        *,
        expected_check_changes: int,
    ) -> tuple[bool, int]:
        """Return whether the host reached its declared safe state."""
        ...


def _data() -> dict[str, Any]:
    """Return one ARC producer fixture."""
    return {
        "runner_image": {"source_host": "producer"},
        "hosts": {
            "producer": {
                "class": "arc_k8s",
                "runners": {"instances": 1},
                "provisions": ["one", "two"],
            }
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


def _clean() -> frp.CommandResult:
    """Return the accepted two-play ARC check evidence."""
    row = "producer : ok=9 changed={} unreachable=0 failed=0 skipped=1 rescued=0 ignored=0\n"
    return frp.CommandResult(0, row.format(2) + row.format(0), "")


def _success_and_failures(apply_host: ApplyHost, failures: list[str]) -> None:
    """Prove successful order and activation-check failure quarantine."""
    data = _data()
    calls: list[tuple[str, str]] = []

    def success(argv: Sequence[str]) -> frp.CommandResult:
        verb, host = _identity(argv)
        calls.append((verb, host))
        return (
            _clean()
            if verb in {"parked-check", "activation-check"}
            else frp.CommandResult(0, "", "")
        )

    if not apply_host(data, "producer", success, expected_check_changes=2)[0]:
        failures.append("ARC declarative activation failed")
    expected = [
        ("parked-apply", "producer"),
        ("parked-check", "producer"),
        ("activate", "producer"),
        ("activation-check", "producer"),
        ("restore", "producer"),
    ]
    if calls != expected:
        failures.append("ARC activation/check/restore order drifted")
    calls.clear()

    def failed_check(argv: Sequence[str]) -> frp.CommandResult:
        verb, host = _identity(argv)
        calls.append((verb, host))
        if verb == "parked-check":
            return _clean()
        return frp.CommandResult(1 if verb == "activation-check" else 0, "", "")

    if apply_host(data, "producer", failed_check, expected_check_changes=2)[0]:
        failures.append("failed held ARC activation check passed")
    if calls[-2:] != [("activation-check", "producer"), ("quarantine", "producer")]:
        failures.append("failed ARC activation check did not retain zero")


def _hard_kill_cut(apply_host: ApplyHost, failures: list[str]) -> None:
    """Prove controller death before validation leaves marker and live zero."""

    class SimulatedHardKill(BaseException):
        """Model controller death after held declarative activation."""

    authority = {"marker": True, "live_zero": True, "helm_declared": False}
    calls: list[tuple[str, str]] = []

    def kill(argv: Sequence[str]) -> frp.CommandResult:
        verb, host = _identity(argv)
        calls.append((verb, host))
        if verb == "parked-check":
            return _clean()
        if verb == "activate":
            authority["helm_declared"] = True
        if verb == "activation-check":
            if not authority["live_zero"] or not authority["marker"]:
                failures.append("ARC activation check ran without held zero admission")
            raise SimulatedHardKill
        return frp.CommandResult(0, "", "")

    try:
        apply_host(_data(), "producer", kill, expected_check_changes=2)
        failures.append("post-activation hard-kill cut returned")
    except SimulatedHardKill:
        pass
    if not all(authority.values()):
        failures.append("hard kill lost marker, zero ceiling, or Helm authority")
    if calls[-2:] != [("activate", "producer"), ("activation-check", "producer")]:
        failures.append("ARC hard-kill cut did not precede sole opener")


def run(apply_host: ApplyHost) -> list[str]:
    """Return every ARC activation state-machine failure."""
    failures: list[str] = []
    _success_and_failures(apply_host, failures)
    _hard_kill_cut(apply_host, failures)
    return failures
