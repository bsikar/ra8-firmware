# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Invoke the declared host-local runner-capacity authority."""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Any, Protocol

sys.path.insert(0, str(Path(__file__).resolve().parent))

import fleet_model as fm
import fleet_reach as fr

CAPACITY_SCRIPT = fm.REPO_ROOT / "scripts" / "ci" / "fleet_capacity.sh"


class CommandRunner(Protocol):
    """Run an exact command while supplying its trusted stdin."""

    def __call__(self, argv: list[str], stdin: str) -> int:
        """Return the child command status."""
        ...


def _host(data: dict[str, Any], name: str) -> dict[str, Any]:
    """Return one declared capacity host."""
    if name not in data["hosts"]:
        message = f"no host '{name}' in infra/fleet.yml"
        raise fm.FleetError(message)
    return data["hosts"][name]


def _fail(message: str) -> int:
    """Emit one capacity precondition failure."""
    print(f"fleet: error: {message}", file=sys.stderr)
    return 2


def policy_flags(host: dict[str, Any]) -> list[str]:
    """Return the exact declared capacity and quiet-hours transport flags."""
    flags = ["--full-instances", str(host["runners"]["instances"])]
    quiet = host.get("quiet_hours") or {}
    if not quiet:
        return flags
    quiet_start, separator, quiet_end = str(quiet["window"]).partition("-")
    if separator != "-":
        message = "quiet-hours window has no start/end separator"
        raise fm.FleetError(message)
    return [
        *flags,
        "--quiet-instances",
        str(quiet["instances"]),
        "--quiet-start",
        quiet_start,
        "--quiet-end",
        quiet_end,
        "--quiet-days",
        str(quiet["days"]),
    ]


def state_group(host: dict[str, Any]) -> str:
    """Return the account group that executes the host-local capacity script."""
    return str(host["connect"]["user"])


def run_selftest(data: dict[str, Any]) -> list[str]:
    """Prove exact streamed capacity argv and refuse malformed quiet hours."""
    failures: list[str] = []
    arc = data["hosts"]["k3s-pve"]
    expected = ["--full-instances", str(arc["runners"]["instances"])]
    if policy_flags(arc)[:2] != expected:
        failures.append("ARC restore argv lost its declared capacity")
    if state_group(arc) != arc["connect"]["user"]:
        failures.append("SSH capacity state lost its connecting account group")
    malformed = {**arc, "quiet_hours": {"window": "18:00"}}
    try:
        policy_flags(malformed)
    except fm.FleetError:
        pass
    else:
        failures.append("malformed quiet-hours transport was accepted")
    return failures


def run(data: dict[str, Any], name: str, args: list[str], command_runner: CommandRunner) -> int:
    """Run ``fleet_capacity.sh`` on a host, over that host's transport.

    The script is piped from the checkout on every call rather than invoked
    from a copy on the host, so an operator command always runs the version in
    the tree. The copy the ``fleet_capacity`` role installs exists for the
    unattended quiet-hours timer, which has no checkout to read from.

    Args:
        data: The parsed declaration.
        name: Fleet host name.
        args: Arguments after the fixed configuration flags.
        command_runner: Exact subprocess transport supplied by the dispatcher.

    Returns:
        The script's exit status.
    """
    host = _host(data, name)
    cls = fm.CLASSES[host["class"]]
    if cls.capacity_kind == "none":
        return _fail(f"{name} is a {host['class']} host and carries no runners to scale")
    state_group_name = state_group(host)
    flags = [
        "--kind",
        cls.capacity_kind,
        "--state-group",
        state_group_name,
        *policy_flags(host),
    ]
    flags += ["--scale-set", host["runners"]["labels"][0]]
    # The capacity script's flags are shaped so none ever needs quoting.
    remote = f"{fm.remote_shell(host)} -- {' '.join(flags)} {' '.join(args)}"
    return command_runner(
        [*fr.ssh_target(data, name), remote],
        stdin=CAPACITY_SCRIPT.read_text(encoding="utf-8"),
    )
