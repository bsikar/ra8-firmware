#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""The schema, arithmetic and derivations behind ``infra/fleet.yml``.

The CLI and declaration gate both import this model, keeping provisioning,
capacity arithmetic, role variables, inventory and validation behind one
definition. Machine reachability is isolated in :mod:`fleet_reach`; the native
non-capacity HIL listener is isolated in :mod:`fleet_hil`.
"""

from __future__ import annotations

import os
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))

import fleet_hil as fh
import fleet_reach as fr
import fleet_runner_model as frm

CLASSES = frm.CLASSES

REPO_ROOT = Path(__file__).resolve().parents[2]
FLEET_FILE = REPO_ROOT / "infra" / "fleet.yml"
ANSIBLE_DIR = REPO_ROOT / "infra" / "ansible"


def _inventory_path() -> Path:
    """Select the service's writable inventory without moving source authority."""
    override = os.environ.get("RA8_FLEET_INVENTORY")
    if override is None:
        return ANSIBLE_DIR / "inventory" / "hosts.ini"
    path = Path(override)
    if not path.is_absolute():
        message = "RA8_FLEET_INVENTORY must be an absolute path"
        raise ValueError(message)
    return path


INVENTORY = _inventory_path()

# Beside the inventory, not beside the playbooks. Ansible auto-loads host_vars
# from the inventory SOURCE's directory; a host_vars tree anywhere else is
# silently never read, which presents as a role running with its bare defaults
# on a host that plainly declares otherwise.
HOST_VARS_DIR = ANSIBLE_DIR / "inventory" / "host_vars"


def validate_runtime_inventory(state_dir: Path) -> None:
    """Bind installed inventory writes beside the immutable host-variable source."""
    expected = state_dir / "inventory" / "hosts.ini"
    if expected != INVENTORY:
        message = "installed reconciliation inventory is outside its private state directory"
        raise ValueError(message)
    host_vars = expected.parent / "host_vars"
    if not host_vars.is_symlink() or host_vars.readlink() != HOST_VARS_DIR:
        message = "runtime inventory host variables are not bound to the immutable source"
        raise ValueError(message)


@dataclass(frozen=True)
class Play:
    """One provisioning play: a playbook, the group it targets, and its roles.

    Attributes:
        playbook: File name under ``infra/ansible/playbooks/``.
        group: Inventory group the play's ``hosts:`` selects.
        roles: Roles the play applies, in order, for ``infra-list``.
        removable: Whether the roles genuinely implement a teardown path.
            Claiming one that does not exist is worse than admitting there is
            none, so this is only true where ``state=absent`` is implemented.
        summary: One line for the listing.
    """

    playbook: str
    group: str
    roles: tuple[str, ...]
    removable: bool
    summary: str


PLAYS: dict[str, Play] = {
    "dev-box": Play(
        playbook="dev-box.yml",
        group="dev_boxes",
        roles=("dev_box",),
        removable=False,
        summary="the pinned host toolchain",
    ),
    "k3s-node": Play(
        playbook="k3s-node.yml",
        group="k3s_nodes",
        roles=("k3s_node", "openbao"),
        removable=False,
        summary="k3s + helm + the vault",
    ),
    "hil-bench": Play(
        playbook="hil-bench.yml",
        group="hil_bench",
        roles=("hil_bench", "c6_toolchain", "ad2_tools"),
        removable=False,
        summary="the HIL bench Pi, ESP32-C6 and AD2",
    ),
}


class FleetError(Exception):
    """A fleet declaration could not be read or does not describe a real fleet."""


def load(path: Path = FLEET_FILE) -> dict[str, Any]:
    """Read and structurally check ``infra/fleet.yml``.

    Args:
        path: Declaration to read. Overridden only by the selftest.

    Returns:
        The parsed mapping, with ``hosts`` guaranteed present.

    Raises:
        FleetError: The file is missing, is not a mapping, or has no
            ``hosts`` mapping.
    """
    if not path.is_file():
        msg = f"no fleet declaration at {path}"
        raise FleetError(msg)
    data = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        msg = f"{path} does not parse to a mapping"
        raise FleetError(msg)
    if not isinstance(data.get("hosts"), dict):
        msg = f"{path} has no 'hosts:' mapping"
        raise FleetError(msg)
    return data


def role_vars(data: dict[str, Any], name: str, host: dict[str, Any]) -> dict[str, Any]:
    """Every Ansible variable derived from one host's declared block.

    Extra-vars beat ``host_vars``; the declaration gate rejects a committed
    duplicate. Roles retain policy defaults, while a fleet-owned identity may
    default empty so standalone execution fails instead of drifting.

    Args:
        data: The complete fleet declaration, including the canonical image.
        name: Fleet host name.
        host: That host's declaration.

    Returns:
        Variable name to value, ready to hand to ``ansible-playbook -e``.
    """
    del name
    return dict(fh.runner_vars(data, host))


def inventory_entry(data: dict[str, Any], name: str) -> str:
    """One inventory line for a host.

    Args:
        data: The parsed declaration.
        name: Fleet host name.

    Returns:
        The ``<name> ansible_host=... ansible_user=...`` line.
    """
    host = data["hosts"][name]
    connect = host["connect"]
    entry = f"{name} ansible_host={connect['address']}"
    if connect.get("user"):
        entry += f" ansible_user={connect['user']}"
    hops = fr.jump_chain(data, name)
    if hops:
        # Ansible reaches a jumped host through its own ssh invocation, not
        # through this module's, so the hops have to be handed to it too --
        # otherwise a converge would be the one path that still needed an alias
        # in somebody's ~/.ssh/config to work.
        entry += f" ansible_ssh_common_args='-o ProxyJump={','.join(hops)}'"
    return entry


def controller_inventory_entry() -> str:
    """Return an explicit localhost entry only for the private service runtime."""
    value = os.environ.get("ANSIBLE_LOCAL_TEMP")
    if value is None:
        return ""
    path = Path(value)
    safe = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/._-"
    if (
        not path.is_absolute()
        or str(path) != value
        or ".." in path.parts
        or any(character not in safe for character in value)
    ):
        message = "ANSIBLE_LOCAL_TEMP cannot be represented safely in inventory"
        raise ValueError(message)
    return f"localhost ansible_connection=local ansible_remote_tmp={value}"


def render_inventory(data: dict[str, Any]) -> str:
    """Generate the Ansible inventory from the declaration.

    Args:
        data: The parsed declaration.

    Returns:
        An INI inventory body, one group per play group.
    """
    groups: dict[str, list[str]] = {}
    for name, host in data["hosts"].items():
        entry = inventory_entry(data, name)
        for play in host["provisions"]:
            group = groups.setdefault(PLAYS[play].group, [])
            if entry not in group:
                group.append(entry)
    lines = [
        "# GENERATED by scripts/dev/fleet.py from infra/fleet.yml -- do not edit.",
        "# Add or retune a machine by editing that file; this is rewritten from",
        "# it on every `just infra::*` run.",
        "",
    ]
    controller = controller_inventory_entry()
    if controller:
        lines.extend(["[fleet_controller]", controller, ""])
    for group_name in sorted(groups):
        lines.append(f"[{group_name}]")
        lines.extend(sorted(groups[group_name]))
        lines.append("")
    return "\n".join(lines)


def inventory_label() -> Path:
    """Return a concise checkout-relative or exact runtime inventory path."""
    try:
        return INVENTORY.relative_to(REPO_ROOT)
    except ValueError:
        return INVENTORY


def controller_inventory_selftest(data: dict[str, Any]) -> list[str]:
    """Prove localhost uses the private service temp without inventory injection."""
    failures: list[str] = []
    previous = os.environ.get("ANSIBLE_LOCAL_TEMP")
    try:
        with tempfile.TemporaryDirectory(prefix="ra8-controller-inventory-") as raw:
            local_temp = Path(raw) / "ansible-local"
            local_temp.mkdir()
            os.environ["ANSIBLE_LOCAL_TEMP"] = str(local_temp)
            expected = f"localhost ansible_connection=local ansible_remote_tmp={local_temp}"
            if render_inventory(data).count(expected) != 1:
                failures.append("private localhost remote temp was absent from inventory")
            os.environ["ANSIBLE_LOCAL_TEMP"] = f"{local_temp}\n[forged]"
            try:
                render_inventory(data)
                failures.append("unsafe localhost remote temp entered inventory")
            except ValueError:
                pass
    finally:
        if previous is None:
            os.environ.pop("ANSIBLE_LOCAL_TEMP", None)
        else:
            os.environ["ANSIBLE_LOCAL_TEMP"] = previous
    return failures


def _check_shape(name: str, host: dict[str, Any]) -> list[str]:
    """Rule: a host names a real class, real plays, and a way to be reached.

    Args:
        name: Fleet host name.
        host: That host's declaration.

    Returns:
        One message per violation.
    """
    bad = []
    if host.get("class") not in CLASSES:
        return [f"{name}: class '{host.get('class')}' is not one of {sorted(CLASSES)}"]
    provisions = host.get("provisions") or []
    if not provisions:
        bad.append(
            f"{name}: provisions is empty, so `just infra::apply HOST={name}` would do nothing"
        )
    bad += [
        f"{name}: provisions '{p}' is not a known play {sorted(PLAYS)}"
        for p in provisions
        if p not in PLAYS
    ]
    return bad


def _check_no_capacity(name: str, host: dict[str, Any]) -> list[str]:
    """Rule: no host declares runner capacity; the fleet has no runner pool.

    Args:
        name: Fleet host name.
        host: That host's declaration.

    Returns:
        One message per capacity key the host still carries.
    """
    return [
        f"{name}: '{key}:' declares runner capacity, and the fleet no longer has a runner pool"
        for key in ("runners", "budget", "quiet_hours", "sizing_note")
        if key in host
    ]


def _check_host_vars(data: dict[str, Any], host_vars_dir: Path) -> list[str]:
    """Rule: no committed ``host_vars`` file re-declares a fleet-owned tunable.

    Extra-vars beat ``host_vars``, so a duplicate would not change what runs --
    it would do something worse: leave a number in the tree that looks
    authoritative, that someone will edit, and that will have no effect. One
    knob, one home.

    Args:
        data: The parsed declaration.
        host_vars_dir: Directory of committed per-host variable files.

    Returns:
        One message per re-declared variable.
    """
    owned: set[str] = set()
    for name, host in data["hosts"].items():
        owned |= set(role_vars(data, name, host))
    bad = []
    for path in sorted(host_vars_dir.glob("*.yml")):
        loaded = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
        bad.extend(
            f"{path.name}: re-declares '{key}', which infra/fleet.yml owns. "
            "Extra-vars beat host_vars, so this value would silently do nothing; "
            "delete it and tune the host's fleet.yml block instead."
            for key in sorted(set(loaded) & owned)
        )
    return bad


def validate(data: dict[str, Any], host_vars_dir: Path | None = None) -> list[str]:
    """Every rule the declaration must satisfy, in one pass.

    Args:
        data: The parsed declaration.
        host_vars_dir: Committed per-host variable files to cross-check against.
            Defaults to the tree's; the selftest points it at a fixture.

    Returns:
        One message per violation, empty when the fleet is well declared.
    """
    problems: list[str] = []
    problems += fh.check_uniqueness(data["hosts"])
    for name, host in data["hosts"].items():
        shape = _check_shape(name, host) + fr.check_connect(name, host, data["hosts"])
        problems += shape
        if shape or host.get("class") not in CLASSES:
            continue
        problems += fh.check_runner(name, host, data["hosts"])
        problems += _check_no_capacity(name, host)
    if not problems:
        # Only once the declaration itself is sound: role_vars() derives the
        # owned-name set from it, so running this over a broken declaration
        # would report a fabricated overlap.
        problems += _check_host_vars(data, host_vars_dir or HOST_VARS_DIR)
    return problems
