# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Host-class naming and transport derivations for the fleet model."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class HostClass:
    """Describe the transport and role of one host class.

    Attributes:
        transport: Remote transport; only ``ssh`` remains.
        summary: One-line human-readable description.
    """

    transport: str
    summary: str


CLASSES: dict[str, HostClass] = {
    "k8s_node": HostClass(
        transport="ssh",
        summary="the single-node k3s cluster that hosts the vault",
    ),
    "dev_box": HostClass(
        # The repo-scoped HIL listener is declared by the host's hil_runner
        # block; it is the only runner the fleet still carries.
        transport="ssh",
        summary="the shared verification box and dedicated HIL listener",
    ),
    "hil_bench": HostClass(
        transport="ssh",
        summary="the hardware-in-the-loop bench",
    ),
}
