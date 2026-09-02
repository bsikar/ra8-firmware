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

import sys
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
        roles=("ci_runner_docker", "dev_slice", "fleet_capacity"),
        removable=True,
        summary="long-lived runner containers on a Docker host",
    ),
    "wsl-ci-host": Play(
        playbook="wsl-ci-host.yml",
        group="wsl_ci_hosts",
        roles=("wsl_ci_host", "ci_runner_docker", "dev_slice", "fleet_capacity"),
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


# Ansible tags whose task set cannot stop, start or recreate a container, so a
# converge limited to them needs no drain and costs the host no runner time.
#
# This is a whitelist rather than a judgement call at the call site: a tag
# added here that DOES touch a container would silently make `fleet.py apply`
# cancel jobs, which is the one failure the drain exists to prevent.
NO_DRAIN_TAGS = frozenset({"capacity", "dev-slice"})

# systemd's default CPUWeight, which is what `system.slice` carries -- and
# every Docker container is a scope under `system.slice` unless it is given an
# explicit `--cgroup-parent`. A dev slice is only a LOW-priority slice if its
# weight is below this, so the number is named here and checked rather than
# left as folklore in a role.
SYSTEMD_DEFAULT_CPU_WEIGHT = 100

# Upper bound cgroup v2 accepts for cpu.weight.
CGROUP_MAX_CPU_WEIGHT = 10000

# The systemd slice unit the dev_slice role installs.
#
# NO DASH IN THE NAME, and that is load-bearing rather than a style choice.
# systemd reads `-` in a slice name as HIERARCHY: a unit called
# `ra8-dev.slice` is created as a child of an auto-generated `ra8.slice`, which
# systemd gives the DEFAULT weight. The dev slice's CPUWeight would then be
# compared against its siblings inside `ra8.slice` -- of which there are none
# -- while `ra8.slice` itself competed with `system.slice` at 100 against 100,
# i.e. the runners and the dev work splitting the machine evenly. Verified on
# the host: `systemd-run --slice=ra8-dev.slice` lands in
# `/ra8.slice/ra8-dev.slice/...`. A single-token name is a root-level slice and
# a direct sibling of `system.slice`, which is the comparison that matters.
#
# Named here because fleet.py has to pass it to the capacity script and the
# role has to create it; the fleet-declaration gate asserts the role's default
# agrees rather than trusting this copy.
DEV_SLICE_UNIT = "ra8dev.slice"


class FleetError(Exception):
    """A fleet declaration could not be read or does not describe a real fleet."""


def load(path: Path = FLEET_FILE) -> dict[str, Any]:
    """Read and structurally check ``infra/fleet.yml``.

    Args:
        path: Declaration to read. Overridden only by the selftest.

    Returns:
        The parsed mapping, with ``sizing``, ``runner_image`` and ``hosts``
        guaranteed present.

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
    for key in ("sizing", "runner_image", "hosts"):
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
    return frm.instance_names(name, host)


def container_names(host: dict[str, Any]) -> list[str]:
    """Docker container names for this host's instances, in instance order.

    Args:
        host: One host's declaration.

    Returns:
        Container names the capacity script drains, empty for a non-container
        class.
    """
    return frm.container_names(host)


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
    return frm.remote_shell(host)


def docker_command(host: dict[str, Any]) -> str:
    """How the capacity script must invoke Docker on this host.

    Args:
        host: One host's declaration.

    Returns:
        ``docker`` where the connecting user owns the socket, ``sudo docker``
        otherwise.
    """
    return frm.docker_command(host)


def _runner_vars(name: str, host: dict[str, Any]) -> dict[str, Any]:
    """Ansible variables carrying this host's declared runner capacity.

    Args:
        name: Fleet host name.
        host: That host's declaration.

    Returns:
        The role variables for the host's class, empty for a non-runner class.
    """
    if not CLASSES[host["class"]].capacity_runner:
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


def _runner_image_vars(data: dict[str, Any], host: dict[str, Any]) -> dict[str, Any]:
    """Map the one declared runner artifact onto its producer and consumers.

    Args:
        data: The complete fleet declaration.
        host: One host's declaration.

    Returns:
        Image variables for the ARC producer or a Docker consumer, empty for a
        machine that neither builds nor runs the shared image.
    """
    image = data["runner_image"]
    if host["class"] == "arc_k8s":
        return {
            "ci_runner_image": image["image"],
            "ci_runner_image_archive": image["archive"],
        }
    if CLASSES[host["class"]].capacity_kind == "docker":
        return {
            "ci_runner_docker_image": image["image"],
            "ci_runner_docker_image_source_host": image["source_host"],
            "ci_runner_docker_image_source_archive": image["archive"],
            "ci_runner_docker_image_source_ssh": fr.ssh_target(data, image["source_host"]),
        }
    return {}


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


def _dev_slice_vars(host: dict[str, Any]) -> dict[str, Any]:
    """Ansible variables for the low-priority dev slice, if one is declared.

    A runner host is a CI host first. The slice it lends to agents is therefore
    declared as a CPU *weight* (it consumes whatever CI is not using and yields
    the moment a job arrives) and a HARD memory cap (memory does not yield, so
    it has to be taken out of CI's reservation up front).

    ``dev_slice_enabled`` is false for a host with no block, so the role
    REMOVES a slice a previous declaration installed. Deleting a block must
    undo it, not orphan a cgroup nobody can account for.

    Args:
        host: One host's declaration.

    Returns:
        The ``dev_slice`` role variables, empty for a class that carries none.
    """
    if CLASSES[host["class"]].capacity_kind != "docker":
        return {}
    slice_ = host.get("dev_slice") or {}
    out: dict[str, Any] = {"dev_slice_enabled": bool(slice_)}
    if not slice_:
        return out
    out.update(
        {
            "dev_slice_cpu_weight": int(slice_["cpu_weight"]),
            "dev_slice_memory": f"{slice_['memory_gb']}G",
            "dev_slice_swap": f"{slice_.get('swap_gb', 0)}G",
            "dev_slice_max_jobs": int(slice_["max_jobs"]),
            # The weight the slice must stay under to be a low-priority one.
            # Passed rather than assumed by the role, so the role can read the
            # host's REAL system.slice weight back and assert against the same
            # number this validator used.
            "dev_slice_ci_cpu_weight": SYSTEMD_DEFAULT_CPU_WEIGHT,
        }
    )
    return out


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
        # Quiet hours have to reach the dev slice too, or standing the runners
        # down buys the owner nothing -- a gate suite in the slice would go on
        # using the machine. Empty when the host lends no slice.
        out["fleet_capacity_dev_slice"] = DEV_SLICE_UNIT if host.get("dev_slice") else ""
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
    return {
        **_runner_vars(name, host),
        **fh.runner_vars(data, host),
        **_runner_image_vars(data, host),
        **_dev_slice_vars(host),
        **_capacity_vars(host),
    }


def inventory_entry(data: dict[str, Any], name: str) -> str:
    """One inventory line for a host.

    Args:
        data: The parsed declaration.
        name: Fleet host name.

    Returns:
        The ``<name> ansible_host=... ansible_user=...`` line, or a
        ``connection=local`` line for a WSL host, whose play runs inside the
        distro because WSL has no SSH daemon of its own.
    """
    host = data["hosts"][name]
    if CLASSES[host["class"]].transport == "wsl":
        return f"{name} ansible_connection=local"
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
    if CLASSES[host["class"]].transport == "wsl":
        missing = [k for k in ("distro", "windows_user") if not (host.get("connect") or {}).get(k)]
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
    if not cls.capacity_runner:
        return [
            f"{name}: class {host['class']} carries no runners, so '{key}:' is meaningless here"
            for key in ("runners", "budget", "quiet_hours", "dev_slice")
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
    par, per_mem = (
        int(sizing["build_parallelism"]),
        int(sizing["memory_per_instance_gb"]),
    )
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


def _check_dev_slice(name: str, host: dict[str, Any]) -> list[str]:
    """Rule: a lent dev slice cannot take anything CI was promised.

    The slice exists so an agent can verify on a runner host without CI
    noticing, and the two properties that make that true are checked here
    rather than trusted:

    * **CPU is a weight below CI's.** The runner containers live in
      ``system.slice`` at systemd's default weight, so a slice at or above that
      would not yield to a job -- it would split the machine with one.
    * **Memory is taken out of CI's reservation, not shared with it.** Memory
      does not yield: a page a dev build holds is a page a job cannot have. So
      the slice's cap plus every runner's cap must fit the budget, exactly as
      the runners alone must.

    Args:
        name: Fleet host name.
        host: That host's declaration.

    Returns:
        One message per violation.
    """
    slice_ = host.get("dev_slice")
    if not slice_:
        return []
    if CLASSES[host["class"]].capacity_kind != "docker":
        return [
            f"{name}: class {host['class']} runs no dev slice -- it is a cgroup on a "
            "Docker host, and there is no role that would create one here"
        ]
    bad = [
        f"{name}: dev_slice.{key} is required (see the dev_slice note in infra/fleet.yml)"
        for key in ("cpu_weight", "memory_gb", "max_jobs")
        if not isinstance(slice_.get(key), int)
    ]
    if bad:
        return bad
    weight = int(slice_["cpu_weight"])
    if not 1 <= weight <= CGROUP_MAX_CPU_WEIGHT:
        bad.append(f"{name}: dev_slice.cpu_weight must be 1..{CGROUP_MAX_CPU_WEIGHT}, got {weight}")
    elif weight >= SYSTEMD_DEFAULT_CPU_WEIGHT:
        bad.append(
            f"{name}: dev_slice.cpu_weight {weight} is not below the "
            f"{SYSTEMD_DEFAULT_CPU_WEIGHT} that system.slice -- where every runner "
            "container lives -- carries, so dev work would compete with CI rather "
            "than yield to it. That is the whole property the slice is for."
        )
    if int(slice_["max_jobs"]) < 1:
        bad.append(f"{name}: dev_slice.max_jobs must be at least 1")
    budget, run = host["budget"], host["runners"]
    reserved = int(run["instances"]) * int(run["memory_gb"])
    lent = int(slice_["memory_gb"])
    if lent < 1:
        bad.append(f"{name}: dev_slice.memory_gb must be at least 1")
    elif reserved + lent > int(budget["memory_gb"]):
        bad.append(
            f"{name}: {run['instances']} runner(s) x {run['memory_gb']} GB reserve "
            f"{reserved} GB and the dev slice caps at {lent} GB, which is "
            f"{reserved + lent} of a {budget['memory_gb']} GB budget. Memory does not "
            "yield, so the slice must fit what the runners leave -- lower "
            "dev_slice.memory_gb or raise budget.memory_gb."
        )
    swap = slice_.get("swap_gb", 0)
    if not isinstance(swap, int) or swap < 0:
        bad.append(f"{name}: dev_slice.swap_gb must be a non-negative integer, got {swap!r}")
    elif swap > int(budget.get("swap_gb", 0)):
        bad.append(
            f"{name}: dev_slice.swap_gb {swap} exceeds the {budget.get('swap_gb', 0)} GB "
            "of swap this host's budget declares, so the cap could not be honoured"
        )
    return bad


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
    sizing = data["sizing"]
    image = data["runner_image"]
    problems = [
        f"sizing.{key} must be a positive integer"
        for key in ("build_parallelism", "memory_per_instance_gb")
        if not isinstance(sizing.get(key), int) or sizing[key] <= 0
    ]
    problems += [
        f"runner_image.{key} must be a non-empty string"
        for key in ("source_host", "image", "archive")
        if not isinstance(image.get(key), str) or not image[key].strip()
    ]
    source = image.get("source_host")
    if isinstance(source, str) and source and source not in data["hosts"]:
        problems.append(f"runner_image.source_host '{source}' is not a declared host")
    elif source in data["hosts"] and "ci-runner" not in data["hosts"][source].get("provisions", []):
        problems.append(
            f"runner_image.source_host '{source}' does not provision ci-runner, "
            "so no declared role produces its archive"
        )
    # Every per-host rule below divides by these, so there is nothing further
    # to say about a fleet whose formula constants do not exist.
    if problems:
        return problems
    problems += fh.check_uniqueness(data["hosts"])
    for name, host in data["hosts"].items():
        shape = _check_shape(name, host) + fr.check_connect(name, host, data["hosts"])
        problems += shape
        if shape or host.get("class") not in CLASSES:
            continue
        problems += fh.check_runner(name, host, data["hosts"])
        block = _check_runner_block(name, host)
        problems += block
        # The arithmetic below reads keys the block check has just proved
        # present; running it over an incomplete host would raise rather than
        # report, and a checker that crashes teaches nothing.
        if block or not CLASSES[host["class"]].capacity_runner:
            continue
        problems += _check_fit(name, host)
        problems += _check_sizing(name, host, sizing)
        problems += _check_quiet_hours(name, host)
        problems += _check_dev_slice(name, host)
    if not problems:
        # Only once the declaration itself is sound: role_vars() derives the
        # owned-name set from it, so running this over a broken declaration
        # would report a fabricated overlap.
        problems += _check_host_vars(data, host_vars_dir or HOST_VARS_DIR)
    return problems
