# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Native HIL listener mapping and validation for the fleet declaration.

The native listener belongs to a ``dev_box`` but is not scalable runner
capacity. Keeping its registration identity and remote-bench relationship in
this focused module prevents that distinction from bloating the fleet's
capacity-arithmetic model while preserving one declarative front door.
"""

from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path
from typing import Any

DEV_RUNNER_AUTHORITY = {
    "dev_box_hil_runner_bench_home": "/var/lib/ra8-hil-client",
    "dev_box_hil_runner_bench_user": "ra8-hil",
    "dev_box_hil_runner_env_file": "/etc/ra8/hil-runner.env",
    "dev_box_hil_runner_group": "ra8-hil",
    "dev_box_hil_runner_home": "/var/lib/ra8-hil-runner",
    "dev_box_hil_runner_root": "/opt/ra8-hil-runner",
    "dev_box_hil_runner_service": "ra8-hil-runner.service",
    "dev_box_hil_runner_sha256": "04cf0be1aff4c3ec3554466c39124ca250e3effd8873bb7e8d68535aa9505d5d",
    "dev_box_hil_runner_user": "ra8-hil",
    "dev_box_hil_runner_version": "2.336.0",
    "dev_box_hil_runner_work_dir": "_work",
}
BENCH_AUTHORITY = {
    "hil_bench_arm_gcc_dumpversion": "13.3.1",
    "hil_bench_arm_gcc_prefix": "/opt/arm-gnu-toolchain-13.3",
    "hil_bench_arm_gcc_release": "13.3.rel1",
    "hil_bench_arm_gcc_sha256_aarch64": (
        "c8824bffd057afce2259f7618254e840715f33523a3d4e4294f471208f976764"
    ),
    "hil_bench_jlink_speed": 1000,
    "hil_bench_lock_dir": "/var/lib/ra8-bench",
    "hil_bench_python_context": "/opt/ra8-hil-python-context",
    "hil_bench_python_marker": "/opt/ra8-hil-python/.ra8-lock-sha256",
    "hil_bench_python_venv": "/opt/ra8-hil-python",
    "hil_bench_ref": "dev",
    "hil_bench_repo_url": "git@github.com:bsikar/ra8-firmware.git",
    "hil_bench_uv_cache": "/opt/ra8-uv-cache",
}


def board_policy(interface: dict[str, Any]) -> dict[str, Any]:
    """Return the canonical installed helper policy declaration."""
    return {
        "board_iface": interface["name"],
        "mac": interface["mac"],
        "phc_index": interface["phc_index"],
        "sysfs_device": interface["sysfs_device"],
        "version": 1,
    }


def policy_digest(interface: dict[str, Any]) -> str:
    """Hash the canonical fleet-owned helper policy."""
    payload = json.dumps(board_policy(interface), sort_keys=True, separators=(",", ":")) + "\n"
    return hashlib.sha256(payload.encode()).hexdigest()


def runner_vars(data: dict[str, Any], host: dict[str, Any]) -> dict[str, Any]:
    """Map one declared native HIL listener onto the ``dev_box`` role.

    Args:
        data: Complete fleet declaration, including the referenced bench host.
        host: Candidate dev-box host.

    Returns:
        Fleet-owned ``dev_box_hil_runner_*`` values, empty for a host without
        a native HIL listener.
    """
    if host.get("class") == "hil_bench":
        interface = host["board_interface"]
        return {
            **BENCH_AUTHORITY,
            "hil_bench_arm_gcc_url": (
                "https://developer.arm.com/-/media/Files/downloads/gnu/13.3.rel1/binrel/"
                "arm-gnu-toolchain-13.3.rel1-aarch64-arm-none-eabi.tar.xz"
            ),
            "hil_bench_repo_dir": f"/home/{host['connect']['user']}/ra8-firmware",
            "hil_bench_eth_iface": interface["name"],
            "hil_bench_eth_mac": interface["mac"],
            "hil_bench_eth_sysfs_device": interface["sysfs_device"],
            "hil_bench_eth_phc_index": interface["phc_index"],
        }
    declared = host.get("hil_runner")
    if not declared:
        return {}
    bench = declared["bench"]
    bench_name = str(bench["host"])
    bench_host = data["hosts"][bench_name]
    bench_address = str(bench_host["connect"]["address"])
    interface = bench_host["board_interface"]
    bench_names = [bench_name, *[str(alias) for alias in bench.get("aliases", [])]]
    if bench_address not in bench_names:
        bench_names.append(bench_address)
    return {
        **DEV_RUNNER_AUTHORITY,
        "dev_box_hil_runner_install_stamp": "2.336.0-isolated-v1",
        "dev_box_hil_runner_url": (
            "https://github.com/actions/runner/releases/download/v2.336.0/"
            "actions-runner-linux-x64-2.336.0.tar.gz"
        ),
        "dev_box_hil_runner_name": declared["name"],
        "dev_box_hil_runner_repo_url": declared["repository"],
        "dev_box_hil_runner_labels": ",".join(declared["labels"]),
        "dev_box_hil_runner_bench_alias": bench_name,
        "dev_box_hil_runner_bench_address": bench_address,
        "dev_box_hil_runner_bench_names": bench_names,
        "dev_box_hil_runner_bench_iface": interface["name"],
        "dev_box_hil_runner_bench_mac": interface["mac"],
        "dev_box_hil_runner_bench_sysfs_device": interface["sysfs_device"],
        "dev_box_hil_runner_bench_phc_index": interface["phc_index"],
        "dev_box_hil_runner_bench_policy_sha256": policy_digest(interface),
    }


def _check_board_interface(name: str, host: dict[str, Any]) -> list[str]:
    """Validate one permanent board-interface identity."""
    interface = host.get("board_interface")
    keys = {"name", "mac", "sysfs_device", "phc_index"}
    if not isinstance(interface, dict) or set(interface) != keys:
        return [f"{name}: hil_bench board_interface must contain exactly {sorted(keys)}"]
    iface = interface["name"]
    mac = interface["mac"]
    device = interface["sysfs_device"]
    phc = interface["phc_index"]
    problems = []
    if not isinstance(iface, str) or re.fullmatch(r"[A-Za-z][A-Za-z0-9_-]{0,14}", iface) is None:
        problems.append(f"{name}: board_interface.name is not a physical interface spelling")
    if not isinstance(mac, str) or re.fullmatch(r"[0-9a-f]{2}(?::[0-9a-f]{2}){5}", mac) is None:
        problems.append(f"{name}: board_interface.mac is not canonical lowercase Ethernet")
    if (
        not isinstance(device, str)
        or not device.startswith("/sys/devices/")
        or ".." in Path(device).parts
        or re.fullmatch(r"/[A-Za-z0-9_.:/-]+", device) is None
    ):
        problems.append(f"{name}: board_interface.sysfs_device is not canonical /sys/devices")
    if type(phc) is not int or phc < 0:
        problems.append(f"{name}: board_interface.phc_index must be a non-negative integer")
    return problems


def _check_identity(name: str, declared: dict[str, Any]) -> list[str]:
    """Validate a native listener's registration and workflow identity."""
    bad = [
        f"{name}: hil_runner.{key} must be a non-empty string"
        for key in ("name", "repository", "workflow")
        if not isinstance(declared.get(key), str) or not declared[key].strip()
    ]
    labels = declared.get("labels")
    if (
        not isinstance(labels, list)
        or not labels
        or any(not isinstance(label, str) or not label.strip() for label in labels)
    ):
        bad.append(f"{name}: hil_runner.labels must be a non-empty list of strings")
    elif len(labels) != len(set(labels)):
        bad.append(f"{name}: hil_runner.labels contains a duplicate")
    elif "self-hosted" in labels:
        bad.append(
            f"{name}: hil_runner.labels declares the implicit 'self-hosted' label; "
            "list only the listener's custom labels"
        )

    workflow = declared.get("workflow")
    if isinstance(workflow, str) and workflow.strip():
        workflow_path = Path(workflow)
        if (
            workflow_path.is_absolute()
            or ".." in workflow_path.parts
            or workflow_path.parts[:2] != (".github", "workflows")
            or workflow_path.suffix not in {".yml", ".yaml"}
        ):
            bad.append(
                f"{name}: hil_runner.workflow '{workflow}' is not a repository-relative "
                ".github/workflows/*.yml path"
            )

    repository = declared.get("repository")
    if (
        isinstance(repository, str)
        and repository.strip()
        and (
            not repository.startswith("https://github.com/")
            or any(ch.isspace() for ch in repository)
        )
    ):
        bad.append(f"{name}: hil_runner.repository must be an https://github.com owner/repo URL")
    return bad


def _check_bench(name: str, bench: object, hosts: dict[str, Any]) -> list[str]:
    """Validate the declared relationship to one instrument host."""
    if not isinstance(bench, dict):
        return [f"{name}: hil_runner.bench must be a mapping"]

    bad = []
    bench_name = bench.get("host")
    if not isinstance(bench_name, str) or not bench_name:
        bad.append(f"{name}: hil_runner.bench.host must name a declared hil_bench host")
    elif bench_name not in hosts:
        bad.append(f"{name}: hil_runner.bench.host '{bench_name}' is not declared")
    elif hosts[bench_name].get("class") != "hil_bench":
        bad.append(
            f"{name}: hil_runner.bench.host '{bench_name}' has class "
            f"{hosts[bench_name].get('class')}, not hil_bench"
        )
    aliases = bench.get("aliases", [])
    if not isinstance(aliases, list) or any(
        not isinstance(alias, str) or not alias.strip() for alias in aliases
    ):
        bad.append(f"{name}: hil_runner.bench.aliases must be a list of non-empty strings")
    elif len(aliases) != len(set(aliases)):
        bad.append(f"{name}: hil_runner.bench.aliases contains a duplicate")
    return bad


def check_runner(name: str, host: dict[str, Any], hosts: dict[str, Any]) -> list[str]:
    """Validate one optional native HIL listener and its bench relation.

    Args:
        name: Fleet host name carrying the declaration.
        host: That host's declaration.
        hosts: Complete host mapping used to resolve the bench reference.

    Returns:
        One message per invalid listener field or bench relationship.
    """
    if host["class"] == "hil_bench":
        return _check_board_interface(name, host)
    declared = host.get("hil_runner")
    if declared is None:
        return []
    if host["class"] != "dev_box":
        return [f"{name}: hil_runner is supported only on class dev_box"]
    if not isinstance(declared, dict):
        return [f"{name}: hil_runner must be a mapping"]
    return _check_identity(name, declared) + _check_bench(name, declared.get("bench"), hosts)


def check_uniqueness(hosts: dict[str, Any]) -> list[str]:
    """Reject duplicate native listener registrations or workflow ownership.

    Args:
        hosts: Complete fleet host mapping.

    Returns:
        One message per duplicated registration name or workflow path.
    """
    problems = []
    for key in ("name", "workflow"):
        owners: dict[str, str] = {}
        for host_name, host in hosts.items():
            declared = host.get("hil_runner")
            if not isinstance(declared, dict) or not isinstance(declared.get(key), str):
                continue
            value = declared[key]
            if value in owners:
                problems.append(
                    f"{host_name}: hil_runner.{key} '{value}' is already owned by {owners[value]}"
                )
            else:
                owners[value] = host_name
    return problems
