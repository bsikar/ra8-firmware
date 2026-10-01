# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Runner-class naming and transport derivations for the fleet model."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any


@dataclass(frozen=True)
class HostClass:
    """Describe capacity, budget, and transport behavior for one host class.

    Attributes:
        capacity_runner: Whether the host carries scalable runner capacity.
        budget_mode: The only budget mode this class may declare.
        transport: Remote transport; only ``ssh`` remains.
        capacity_kind: Capacity controller arm used to scale this host.
        summary: One-line human-readable description.
    """

    capacity_runner: bool
    budget_mode: str
    transport: str
    capacity_kind: str
    summary: str


CLASSES: dict[str, HostClass] = {
    "arc_k8s": HostClass(
        capacity_runner=True,
        budget_mode="burst",
        transport="ssh",
        capacity_kind="k8s",
        summary="an ARC scale set on a k8s cluster",
    ),
    "docker_linux": HostClass(
        capacity_runner=True,
        budget_mode="reserved",
        transport="ssh",
        capacity_kind="docker",
        summary="runner containers on a plain Docker host",
    ),
    "dev_box": HostClass(
        # The repo-scoped HIL listener is declared by the host's hil_runner
        # block, but deliberately is not scalable general fleet capacity.
        capacity_runner=False,
        budget_mode="reserved",
        transport="ssh",
        capacity_kind="none",
        summary="the shared verification box and dedicated HIL listener",
    ),
    "hil_bench": HostClass(
        capacity_runner=False,
        budget_mode="reserved",
        transport="ssh",
        capacity_kind="none",
        summary="the hardware-in-the-loop bench",
    ),
}

# The ci_runner_docker role uses this base for its container instance names.
CONTAINER_BASE = "ra8-ci-runner"


def instance_names(name: str, host: dict[str, Any]) -> list[str]:
    """Return the stable runner-registration names for one host."""
    if CLASSES[host["class"]].capacity_kind != "docker":
        return []
    runners = host.get("runners") or {}
    base = runners.get("name", name)
    count = int(runners.get("instances", 0))
    if count == 1:
        return [base]
    return [f"{base}-{index}" for index in range(1, count + 1)]


def container_names(host: dict[str, Any]) -> list[str]:
    """Return Docker container names for one host in instance order."""
    if CLASSES[host["class"]].capacity_kind != "docker":
        return []
    count = int((host.get("runners") or {}).get("instances", 0))
    if count == 1:
        return [CONTAINER_BASE]
    return [f"{CONTAINER_BASE}-{index}" for index in range(1, count + 1)]


def remote_shell(host: dict[str, Any]) -> str:
    """Return the fixed remote command that executes a stdin shell script."""
    del host
    return "/bin/bash -s"


def docker_command(host: dict[str, Any]) -> str:
    """Return the fixed Docker command for one host's transport."""
    del host
    return "sudo docker"
