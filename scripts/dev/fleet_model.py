#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""The schema, arithmetic and derivations behind ``infra/fleet.yml``.

``infra/fleet.yml`` is the one registry of what machines this project runs on.
This module is the only thing that reads it, and everything a host needs
downstream is *derived* here rather than restated anywhere else:

* which playbook(s) provision it, and over which transport,
* which Ansible variables its class maps its declared capacity onto,
* the generated inventory,
* the container names its runner instances will have, which the capacity
  script needs in order to drain them,
* whether the declared instance count is the one the sizing formula gives.

Schema, arithmetic and dispatch live together on purpose. Splitting the
validator from the mapping is how a checker ends up policing a shape the
dispatcher no longer produces -- the exact drift this tree keeps finding in its
own tooling. :mod:`fleet` (the CLI) and the ``fleet-declaration`` gate both
import this module, so there is one definition and two front doors.
"""

from __future__ import annotations

import shlex
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import yaml

REPO_ROOT = Path(__file__).resolve().parents[2]
FLEET_FILE = REPO_ROOT / "infra" / "fleet.yml"
ANSIBLE_DIR = REPO_ROOT / "infra" / "ansible"
INVENTORY = ANSIBLE_DIR / "inventory" / "hosts.ini"

# Beside the inventory, not beside the playbooks. Ansible auto-loads host_vars
# from the inventory SOURCE's directory; a host_vars tree anywhere else is
# silently never read, which presents as a role running with its bare defaults
# on a host that plainly declares otherwise.
HOST_VARS_DIR = ANSIBLE_DIR / "inventory" / "host_vars"

# Days a quiet-hours window may name, in the spelling systemd's OnCalendar
# accepts, so the declaration goes into a timer without translation.
WEEKDAYS = ("Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun")

# Bounds of a 24-hour wall clock, for the quiet-hours window check.
LAST_HOUR = 23
LAST_MINUTE = 59


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
        group="ci_runners",
        roles=("k3s_node", "openbao"),
        removable=False,
        summary="k3s + helm + the vault",
    ),
    "ci-runner": Play(
        playbook="ci-runner.yml",
        group="ci_runners",
        roles=("ci_runner",),
        removable=False,
        summary="the ARC autoscaling runner pool",
    ),
    "ci-runner-docker": Play(
        playbook="ci-runner-docker.yml",
        group="ci_runners_docker",
        roles=("ci_runner_docker", "fleet_capacity"),
        removable=True,
        summary="long-lived runner containers on a Docker host",
    ),
    "wsl-ci-host": Play(
        playbook="wsl-ci-host.yml",
        group="wsl_ci_hosts",
        roles=("wsl_ci_host", "ci_runner_docker", "fleet_capacity"),
        removable=True,
        summary="a Windows machine's WSL2 distro, then the runners into it",
    ),
    "hil-bench": Play(
        playbook="hil-bench.yml",
        group="hil_bench",
        roles=("hil_bench", "c6_toolchain", "ad2_tools"),
        removable=False,
        summary="the HIL bench Pi, ESP32-C6 and AD2",
    ),
}


@dataclass(frozen=True)
class HostClass:
    """What a host's ``class:`` decides.

    Attributes:
        runner: Whether the host carries GitHub Actions runners at all. A dev
            box and the bench do not, so no capacity arithmetic applies to
            them.
        budget_mode: The only ``budget.mode`` this class may declare.
            ``reserved`` means the caps are kernel-enforced reservations that
            must fit the budget; ``burst`` means they are ceilings a scheduler
            may oversubscribe, so the requests are what must fit.
        transport: ``ssh`` for a machine Ansible can reach directly, ``wsl``
            for one where the play has to be copied inside a WSL2 distro and
            run with ``--connection=local``.
        capacity_kind: Which arm of ``scripts/ci/fleet_capacity.sh`` scales it.
        summary: One line for the listing.
    """

    runner: bool
    budget_mode: str
    transport: str
    capacity_kind: str
    summary: str


CLASSES: dict[str, HostClass] = {
    "arc_k8s": HostClass(
        runner=True,
        budget_mode="burst",
        transport="ssh",
        capacity_kind="k8s",
        summary="an ARC scale set on a k8s cluster",
    ),
    "docker_linux": HostClass(
        runner=True,
        budget_mode="reserved",
        transport="ssh",
        capacity_kind="docker",
        summary="runner containers on a plain Docker host",
    ),
    "docker_wsl": HostClass(
        runner=True,
        budget_mode="reserved",
        transport="wsl",
        capacity_kind="docker",
        summary="runner containers inside a Windows WSL2 distro",
    ),
    "dev_box": HostClass(
        runner=False,
        budget_mode="reserved",
        transport="ssh",
        capacity_kind="none",
        summary="the shared verification box",
    ),
    "hil_bench": HostClass(
        runner=False,
        budget_mode="reserved",
        transport="ssh",
        capacity_kind="none",
        summary="the hardware-in-the-loop bench",
    ),
}

# The container the ci_runner_docker role names its instances after. Declared
# here because fleet.py has to predict the names in order to drain them, and
# the role has to produce them; the fleet-declaration gate asserts the two
# agree rather than trusting this copy.
CONTAINER_BASE = "ra8-ci-runner"


class FleetError(Exception):
    """A fleet declaration could not be read or does not describe a real fleet."""


def load(path: Path = FLEET_FILE) -> dict[str, Any]:
    """Read and structurally check ``infra/fleet.yml``.

    Args:
        path: Declaration to read. Overridden only by the selftest.

    Returns:
        The parsed mapping, with ``sizing`` and ``hosts`` guaranteed present.

    Raises:
        FleetError: The file is missing, is not a mapping, or lacks either of
            the two top-level keys everything else derives from.
    """
    if not path.is_file():
        msg = f"no fleet declaration at {path}"
        raise FleetError(msg)
    data = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        msg = f"{path} does not parse to a mapping"
        raise FleetError(msg)
    for key in ("sizing", "hosts"):
        if not isinstance(data.get(key), dict):
            msg = f"{path} has no '{key}:' mapping"
            raise FleetError(msg)
    return data


def recommended_instances(sizing: dict[str, Any], budget: dict[str, Any]) -> int:
    """Instance count the sizing formula gives for a budget.

    ``min(threads / build_parallelism, memory_gb / memory_per_instance_gb)``.
    Both divisors are measured properties of this tree, documented at the top
    of ``infra/fleet.yml``: a job cannot use more CPUs than the workflows'
    pinned build parallelism, and clang-tidy has been OOM-killed below the
    memory divisor.

    Args:
        sizing: The declaration's ``sizing:`` block.
        budget: One host's ``budget:`` block.

    Returns:
        The recommended count, never below zero.
    """
    by_cpu = int(budget["threads"]) // int(sizing["build_parallelism"])
    by_mem = int(budget["memory_gb"]) // int(sizing["memory_per_instance_gb"])
    return max(0, min(by_cpu, by_mem))


def instance_names(name: str, host: dict[str, Any]) -> list[str]:
    """Runner registration names this host's instances will carry on GitHub.

    Mirrors the ``ci_runner_docker`` role exactly: a single instance keeps the
    unsuffixed base name, and above one every instance is ``<base>-<i>``. The
    difference matters because those are the names in
    ``gh api .../actions/runners``, and moving between the two forms renames a
    registration.

    Args:
        name: Fleet host name, the default base.
        host: That host's declaration.

    Returns:
        One name per declared instance, in instance order. Empty for an ARC
        host: its runner names are generated per ephemeral pod by the
        controller, so there is no stable set to predict.
    """
    if CLASSES[host["class"]].capacity_kind != "docker":
        return []
    runners = host.get("runners") or {}
    base = runners.get("name", name)
    count = int(runners.get("instances", 0))
    if count == 1:
        return [base]
    return [f"{base}-{i}" for i in range(1, count + 1)]


def container_names(host: dict[str, Any]) -> list[str]:
    """Docker container names for this host's instances, in instance order.

    Args:
        host: One host's declaration.

    Returns:
        Container names the capacity script drains, empty for a non-container
        class.
    """
    if CLASSES[host["class"]].capacity_kind != "docker":
        return []
    count = int((host.get("runners") or {}).get("instances", 0))
    if count == 1:
        return [CONTAINER_BASE]
    return [f"{CONTAINER_BASE}-{i}" for i in range(1, count + 1)]


def ssh_target(host: dict[str, Any]) -> list[str]:
    """The ``ssh`` argv prefix that reaches a host from the control node.

    Args:
        host: One host's declaration.

    Returns:
        A complete ssh command up to but not including the remote command.
    """
    connect = host["connect"]
    argv = ["ssh", "-o", "ConnectTimeout=15", "-o", "BatchMode=yes"]
    if connect.get("jump"):
        argv += ["-J", str(connect["jump"])]
    argv.append(str(connect["ssh"]))
    return argv


def remote_shell(host: dict[str, Any]) -> str:
    """The remote command that reads a shell script on stdin and runs it.

    A WSL host has no SSH daemon of its own, so the play and every capacity
    command reach the distro through the Windows side's ssh and ``wsl -e``.
    Feeding the script on stdin rather than quoting it into the command line
    keeps it clear of both the Windows shell's parsing and the distro's.

    Args:
        host: One host's declaration.

    Returns:
        A remote command string ending in ``bash -s``.
    """
    if CLASSES[host["class"]].transport == "wsl":
        distro = shlex.quote(str(host["connect"]["distro"]))
        return f"wsl -d {distro} -u root -e bash -s"
    # The NAS deploy user is not in the docker group, and the k3s node needs
    # root for kubectl; sudo is applied inside the script's docker/kubectl
    # invocation rather than to the whole shell, so the script itself never
    # needs privilege it does not use.
    return "bash -s"


def docker_command(host: dict[str, Any]) -> str:
    """How the capacity script must invoke Docker on this host.

    Args:
        host: One host's declaration.

    Returns:
        ``docker`` where the connecting user owns the socket, ``sudo docker``
        otherwise.
    """
    if CLASSES[host["class"]].transport == "wsl":
        return "docker"
    return "sudo docker"


def _runner_vars(name: str, host: dict[str, Any]) -> dict[str, Any]:
    """Ansible variables carrying this host's declared runner capacity.

    Args:
        name: Fleet host name.
        host: That host's declaration.

    Returns:
        The role variables for the host's class, empty for a non-runner class.
    """
    if not CLASSES[host["class"]].runner:
        return {}
    run = host["runners"]
    if host["class"] == "arc_k8s":
        return {
            "ci_runner_max": int(run["instances"]),
            "ci_runner_cpu_limit": str(run["cpus"]),
            "ci_runner_mem_limit": f"{run['memory_gb']}Gi",
            "ci_runner_cpu_request": str(run["cpu_request"]),
            "ci_runner_mem_request": f"{run['memory_request_gb']}Gi",
            "ci_runner_scale_set_name": run["labels"][0],
        }
    memory = f"{run['memory_gb']}g"
    out: dict[str, Any] = {
        "ci_runner_docker_name": run.get("name", name),
        "ci_runner_docker_instances": int(run["instances"]),
        "ci_runner_docker_cpus": str(run["cpus"]),
        "ci_runner_docker_memory": memory,
        # Equal to the memory cap, never larger: a swapping runner is one whose
        # job has quietly become an order of magnitude slower, and that
        # presents as a timeout rather than as the OOM it really is.
        "ci_runner_docker_memswap": memory,
        "ci_runner_docker_pin_cpus": bool(run.get("pin_cpus", False)),
        "ci_runner_docker_labels": ",".join(run["labels"]),
    }
    if host["class"] == "docker_wsl":
        out.update(_wsl_vars(host))
    return out


def _wsl_vars(host: dict[str, Any]) -> dict[str, Any]:
    """The WSL2 VM caps, which are this host's CI budget stated to Windows.

    Args:
        host: One ``docker_wsl`` host's declaration.

    Returns:
        The ``wsl_ci_host`` role variables written into ``.wslconfig``.
    """
    budget = host["budget"]
    connect = host["connect"]
    return {
        "wsl_ci_host_windows_user": connect["windows_user"],
        "wsl_ci_host_distro": connect["distro"],
        "wsl_ci_host_processors": int(budget["threads"]),
        "wsl_ci_host_memory": f"{budget['memory_gb']}GB",
        "wsl_ci_host_swap": f"{budget['swap_gb']}GB",
    }


def _capacity_vars(host: dict[str, Any]) -> dict[str, Any]:
    """Ansible variables the ``fleet_capacity`` role needs to install a timer.

    Args:
        host: One host's declaration.

    Returns:
        The role variables, including the quiet-hours window when one is
        declared. ``fleet_capacity_enabled`` is false for a host with no
        window, so the role removes a timer a previous declaration installed --
        deleting a block must undo it, not orphan it.
    """
    cls = CLASSES[host["class"]]
    if cls.capacity_kind == "none":
        return {}
    quiet = host.get("quiet_hours") or {}
    out: dict[str, Any] = {
        "fleet_capacity_kind": cls.capacity_kind,
        "fleet_capacity_full_instances": int(host["runners"]["instances"]),
        "fleet_capacity_enabled": bool(quiet),
    }
    if cls.capacity_kind == "docker":
        out["fleet_capacity_docker"] = docker_command(host)
        out["fleet_capacity_containers"] = " ".join(container_names(host))
    else:
        out["fleet_capacity_scale_set"] = host["runners"]["labels"][0]
    if quiet:
        start, _, end = str(quiet["window"]).partition("-")
        out.update(
            {
                "fleet_capacity_quiet_instances": int(quiet["instances"]),
                "fleet_capacity_quiet_start": start,
                "fleet_capacity_quiet_end": end,
                "fleet_capacity_quiet_days": str(quiet["days"]),
            }
        )
    return out


def role_vars(name: str, host: dict[str, Any]) -> dict[str, Any]:
    """Every Ansible variable derived from one host's declared block.

    This mapping is what makes ``infra/fleet.yml`` the single home for a knob:
    the roles keep their own defaults so a standalone run still works, and
    these override them as extra-vars, which beat ``host_vars``. The
    ``fleet-declaration`` gate fails a committed ``host_vars`` file that
    re-declares any name produced here, so a tunable cannot come to live in two
    places.

    Args:
        name: Fleet host name.
        host: That host's declaration.

    Returns:
        Variable name to value, ready to hand to ``ansible-playbook -e``.
    """
    return {**_runner_vars(name, host), **_capacity_vars(host)}


def _inventory_entry(name: str, host: dict[str, Any]) -> str:
    """One inventory line for a host.

    Args:
        name: Fleet host name.
        host: That host's declaration.

    Returns:
        The ``<name> ansible_host=... ansible_user=...`` line, or a
        ``connection=local`` line for a WSL host, whose play runs inside the
        distro because WSL has no SSH daemon of its own.
    """
    if CLASSES[host["class"]].transport == "wsl":
        return f"{name} ansible_connection=local"
    target = str(host["connect"]["ssh"])
    user, _, addr = target.rpartition("@")
    entry = f"{name} ansible_host={addr}"
    if user:
        entry += f" ansible_user={user}"
    return entry


def render_inventory(data: dict[str, Any]) -> str:
    """Generate the Ansible inventory from the declaration.

    The inventory used to be hand-copied from an example file, which made the
    set of machines a thing declared twice. It is generated now, so adding a
    host block is the whole of adding a host.

    Args:
        data: The parsed declaration.

    Returns:
        An INI inventory body, one group per play group.
    """
    groups: dict[str, list[str]] = {}
    for name, host in data["hosts"].items():
        entry = _inventory_entry(name, host)
        for play in host["provisions"]:
            group = groups.setdefault(PLAYS[play].group, [])
            if entry not in group:
                group.append(entry)
    lines = [
        "# GENERATED by scripts/dev/fleet.py from infra/fleet.yml -- do not edit.",
        "# Add or retune a machine by editing that file; this is rewritten from",
        "# it on every `make infra-*` run.",
        "",
    ]
    for group_name in sorted(groups):
        lines.append(f"[{group_name}]")
        lines.extend(sorted(groups[group_name]))
        lines.append("")
    return "\n".join(lines)


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
    if not (host.get("connect") or {}).get("ssh"):
        bad.append(f"{name}: connect.ssh is required -- nothing can reach it without one")
    provisions = host.get("provisions") or []
    if not provisions:
        bad.append(
            f"{name}: provisions is empty, so `make infra-apply HOST={name}` would do nothing"
        )
    bad += [
        f"{name}: provisions '{p}' is not a known play {sorted(PLAYS)}"
        for p in provisions
        if p not in PLAYS
    ]
    if CLASSES[host["class"]].transport == "wsl":
        missing = [k for k in ("distro", "windows_user") if not host["connect"].get(k)]
        bad += [f"{name}: a docker_wsl host needs connect.{k}" for k in missing]
    return bad


def _check_runner_block(name: str, host: dict[str, Any]) -> list[str]:
    """Rule: runner classes declare capacity and a budget; others declare none.

    Args:
        name: Fleet host name.
        host: That host's declaration.

    Returns:
        One message per violation.
    """
    cls = CLASSES[host["class"]]
    run, budget = host.get("runners"), host.get("budget")
    if not cls.runner:
        return [
            f"{name}: class {host['class']} carries no runners, so '{key}:' is meaningless here"
            for key in ("runners", "budget", "quiet_hours")
            if host.get(key)
        ]
    bad = []
    if not run:
        bad.append(f"{name}: a runner host must declare runners.instances")
    elif not run.get("labels"):
        bad.append(f"{name}: runners.labels is empty, so no `runs-on:` would ever reach it")
    if not budget:
        bad.append(f"{name}: a runner host must declare a budget (threads, memory_gb)")
    elif budget.get("mode") != cls.budget_mode:
        bad.append(
            f"{name}: budget.mode is '{budget.get('mode')}' but class {host['class']} is "
            f"only honest as '{cls.budget_mode}' -- see the mode note in infra/fleet.yml"
        )
    if cls.transport == "wsl" and budget and "swap_gb" not in budget:
        bad.append(f"{name}: a docker_wsl budget must set swap_gb (the VM's swap file)")
    if run and cls.budget_mode == "burst":
        # A burst host is packed by what it REQUESTS, so the requests are not
        # optional extras -- without them there is no arithmetic to check.
        bad += [
            f"{name}: a burst-mode host must declare runners.{key}"
            for key in ("cpu_request", "memory_request_gb")
            if key not in run
        ]
    return bad


def _check_fit(name: str, host: dict[str, Any]) -> list[str]:
    """Rule: what a host promises its runners must fit what CI may use.

    ``reserved`` caps are kernel-enforced, so the caps themselves must fit;
    ``burst`` caps are ceilings a scheduler may oversubscribe, so the requests
    are what must fit. Applying the reserved arithmetic to a k8s scale set
    would fail a shape that is correct, which is how a gate teaches people to
    ignore it.

    Args:
        name: Fleet host name.
        host: That host's declaration.

    Returns:
        One message per violation.
    """
    run, budget = host["runners"], host["budget"]
    count = int(run["instances"])
    if budget["mode"] == "burst":
        cpu, mem = int(run["cpu_request"]), int(run["memory_request_gb"])
        what = "request"
    else:
        cpu, mem = int(run["cpus"]), int(run["memory_gb"])
        what = "cap"
    bad = []
    if count * cpu > int(budget["threads"]):
        bad.append(
            f"{name}: {count} instances x {cpu} CPU {what} = {count * cpu} exceeds the "
            f"declared budget of {budget['threads']} threads"
        )
    if count * mem > int(budget["memory_gb"]):
        bad.append(
            f"{name}: {count} instances x {mem} GB {what} = {count * mem} exceeds the "
            f"declared budget of {budget['memory_gb']} GB"
        )
    return bad


def _sizing_deviations(host: dict[str, Any], sizing: dict[str, Any]) -> list[str]:
    """Every way a host departs from what the sizing formula would give it.

    Args:
        host: One host's declaration.
        sizing: The declaration's ``sizing:`` block.

    Returns:
        One phrase per departure, empty when the host is sized by the formula.
    """
    run, budget = host["runners"], host["budget"]
    par, per_mem = int(sizing["build_parallelism"]), int(sizing["memory_per_instance_gb"])
    out = []
    if int(run["cpus"]) < par:
        out.append(
            f"{run['cpus']} CPUs per instance is under the pinned build parallelism "
            f"of {par}, so every job would be throttled below its own fan-out"
        )
    if int(run["memory_gb"]) < per_mem:
        out.append(
            f"{run['memory_gb']} GB per instance is under the {per_mem} GB clang-tidy "
            "has been OOM-killed below, and an instance that OOMs mid-job presents as "
            "a flaky gate"
        )
    want = recommended_instances(sizing, budget)
    if int(run["instances"]) != want:
        out.append(
            f"{run['instances']} instances, where min({budget['threads']}/{par}, "
            f"{budget['memory_gb']}/{per_mem}) gives {want}"
        )
    return out


def _check_sizing(name: str, host: dict[str, Any], sizing: dict[str, Any]) -> list[str]:
    """Rule: a host is sized by the formula, or says in writing why it is not.

    The formula is not a hard limit -- three hosts have real reasons to depart
    from it, and pretending otherwise would either force wrong numbers or make
    the rule something people learn to work around. What it does enforce is
    that a departure is DELIBERATE and legible: no number in this fleet may be
    one nobody can re-derive.

    Args:
        name: Fleet host name.
        host: That host's declaration.
        sizing: The declaration's ``sizing:`` block.

    Returns:
        One message when the host departs from the formula with no written
        reason, empty otherwise.
    """
    deviations = _sizing_deviations(host, sizing)
    if not deviations or str(host.get("sizing_note", "")).strip():
        return []
    joined = "; ".join(deviations)
    return [
        f"{name}: departs from the sizing formula ({joined}) with no sizing_note. "
        "Either use the formula's numbers or write down why not."
    ]


def _check_quiet_hours(name: str, host: dict[str, Any]) -> list[str]:
    """Rule: a declared quiet-hours window is one a timer can actually be built from.

    Args:
        name: Fleet host name.
        host: That host's declaration.

    Returns:
        One message per violation.
    """
    quiet = host.get("quiet_hours")
    if not quiet:
        return []
    bad = []
    window = str(quiet.get("window", ""))
    start, sep, end = window.partition("-")
    if not sep or not all(_is_hhmm(part) for part in (start, end)):
        bad.append(f"{name}: quiet_hours.window '{window}' is not HH:MM-HH:MM")
    days = [d.strip() for d in str(quiet.get("days", "")).split(",") if d.strip()]
    if not days:
        bad.append(f"{name}: quiet_hours.days is empty; name the weekdays it applies to")
    bad += [
        f"{name}: quiet_hours.days '{d}' is not one of {list(WEEKDAYS)}"
        for d in days
        if d not in WEEKDAYS
    ]
    declared = int(host["runners"]["instances"])
    target = quiet.get("instances")
    if not isinstance(target, int) or not 0 <= target < declared:
        bad.append(
            f"{name}: quiet_hours.instances must be 0..{declared - 1} (it is a REDUCTION "
            f"from the declared {declared}); got {target!r}"
        )
    return bad


def _is_hhmm(text: str) -> bool:
    """Whether a string is a 24-hour ``HH:MM`` time.

    Args:
        text: Candidate.

    Returns:
        True when systemd's ``OnCalendar`` would accept it as a time of day.
    """
    hours, _, minutes = text.strip().partition(":")
    if not (hours.isdigit() and minutes.isdigit()):
        return False
    return 0 <= int(hours) <= LAST_HOUR and 0 <= int(minutes) <= LAST_MINUTE


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
        owned |= set(role_vars(name, host))
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
    sizing = data["sizing"]
    problems = [
        f"sizing.{key} must be a positive integer"
        for key in ("build_parallelism", "memory_per_instance_gb")
        if not isinstance(sizing.get(key), int) or sizing[key] <= 0
    ]
    # Every per-host rule below divides by these, so there is nothing further
    # to say about a fleet whose formula constants do not exist.
    if problems:
        return problems
    for name, host in data["hosts"].items():
        shape = _check_shape(name, host)
        problems += shape
        if shape or host.get("class") not in CLASSES:
            continue
        block = _check_runner_block(name, host)
        problems += block
        # The arithmetic below reads keys the block check has just proved
        # present; running it over an incomplete host would raise rather than
        # report, and a checker that crashes teaches nothing.
        if block or not CLASSES[host["class"]].runner:
            continue
        problems += _check_fit(name, host)
        problems += _check_sizing(name, host, sizing)
        problems += _check_quiet_hours(name, host)
    if not problems:
        # Only once the declaration itself is sound: role_vars() derives the
        # owned-name set from it, so running this over a broken declaration
        # would report a fabricated overlap.
        problems += _check_host_vars(data, host_vars_dir or HOST_VARS_DIR)
    return problems
