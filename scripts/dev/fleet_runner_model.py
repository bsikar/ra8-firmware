# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Runner-class naming and transport derivations for the fleet model."""

from __future__ import annotations

from dataclasses import dataclass


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
