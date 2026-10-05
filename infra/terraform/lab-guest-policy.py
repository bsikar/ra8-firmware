#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

"""Read-only Proxmox identity gates for the disposable OpenTofu guest."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from typing import Any

NODE = "pve1"
TEMPLATE_ID = 9001
TEMPLATE_NAME = "ra8-lab-debian-template"
TEMPLATE_MARKER = "RA8_LAB_TEMPLATE=linux-ci-v1"
VM_ID = 9020
BRIDGE = "vmbr9"
POOL = "ra8-tf-lab"
DATASTORE = "ra8-tf-lab"
RUN_RE = re.compile(r"^[0-9a-f]{16}$")
DIGEST_RE = re.compile(r"^[0-9a-f]{40}$")
DISK_RE = re.compile(r"(?:^|,)size=([0-9]+)([KMG])(?:i?B)?(?:,|$)")


class BoundaryError(ValueError):
    """A Proxmox fact fell outside the reviewed lifecycle boundary."""


def _require(condition: bool, label: str) -> None:
    if not condition:
        raise BoundaryError(label)


def _storage_name(volume: str) -> str:
    return volume.split(":", 1)[0] if ":" in volume else ""


def _disk_mib(config: dict[str, Any]) -> int:
    total = 0
    for key, raw in config.items():
        if not re.match(r"^(?:scsi|virtio|sata|ide|efidisk|tpmstate|unused)[0-9]+$", key):
            continue
        if not isinstance(raw, str) or raw == "cdrom,media=cdrom":
            continue
        _require(_storage_name(raw) == DATASTORE, "disk is outside the allowlisted datastore")
        match = DISK_RE.search(raw)
        if match is None:
            continue
        size, unit = int(match.group(1)), match.group(2)
        total += size * {"K": 1 / 1024, "M": 1, "G": 1024}[unit]
    return int(total)


def check_static(vmid: int, run_id: str, bridge: str, pool: str, datastore: str) -> None:
    _require(9000 <= vmid <= 9099, "VMID is outside the reserved range")
    _require(vmid == VM_ID, "VMID is not the assigned lifecycle ID")
    _require(RUN_RE.fullmatch(run_id) is not None, "run marker is not canonical")
    _require(bridge == BRIDGE, "guest bridge is not the recipe's vmbr9")
    _require(pool == POOL, "guest pool is not allowlisted")
    _require(datastore == DATASTORE, "guest datastore is not allowlisted")


def check_template(facts: dict[str, Any]) -> str:
    _require(facts.get("vmid") == TEMPLATE_ID, "template ID mismatch")
    _require(facts.get("name") == TEMPLATE_NAME, "template name mismatch")
    _require(facts.get("template") is True, "clone source is not a template")
    _require(facts.get("status") == "stopped", "clone source is not stopped")
    _require(facts.get("pool") == POOL, "template is outside the allowlisted pool")
    _require(TEMPLATE_MARKER in str(facts.get("description", "")), "template readiness marker is missing")
    _require(facts.get("datastore") == DATASTORE, "template disk is outside the allowlisted datastore")
    digest = str(facts.get("digest", ""))
    _require(DIGEST_RE.fullmatch(digest) is not None, "template config digest is invalid")
    return digest


def check_guest(
    facts: dict[str, Any], run_id: str, expected_digest: str | None = None, *, allow_stopped: bool = False
) -> str:
    check_static(int(facts.get("vmid", 0)), run_id, str(facts.get("bridge", "")), str(facts.get("pool", "")), str(facts.get("datastore", "")))
    _require(facts.get("node") == NODE, "guest is on an unapproved node")
    _require(facts.get("name") == f"ra8-lab-linux-{run_id}", "guest name does not match this run")
    _require(facts.get("template") is False, "target VM is a template")
    _require(facts.get("description") == f"Disposable RA8 CI lifecycle guest; RA8_LAB_RUN={run_id}", "guest run marker mismatch")
    _require(f"run-{run_id}" in facts.get("tags", []), "guest run tag mismatch")
    statuses = ("running", "stopped") if allow_stopped else ("running",)
    _require(facts.get("status") in statuses, "guest is not in an allowed lifecycle state")
    _require(int(facts.get("cores", 0)) * int(facts.get("sockets", 1)) <= 4, "guest exceeds 4 vCPU")
    _require(int(facts.get("memory", 0)) <= 8192, "guest exceeds 8192 MiB memory")
    _require(_disk_mib(facts.get("config", {})) <= 32 * 1024, "guest exceeds 32 GiB disk")
    digest = str(facts.get("digest", ""))
    _require(DIGEST_RE.fullmatch(digest) is not None, "guest config digest is invalid")
    if expected_digest is not None:
        _require(digest == expected_digest, "guest config digest changed after it was bound")
    return digest


def protection_update_args(facts: dict[str, Any], run_id: str, expected_digest: str) -> list[str] | None:
    _require(DIGEST_RE.fullmatch(expected_digest) is not None, "bound guest config digest is invalid")
    check_guest(facts, run_id, expected_digest, allow_stopped=True)
    protection = facts.get("protection", 0)
    _require(protection in (0, 1, False, True, "0", "1"), "guest protection state is invalid")
    if protection in (1, True, "1"):
        return [
            "pvesh", "set", f"/nodes/{NODE}/qemu/{VM_ID}/config",
            "--protection", "0", "--digest", expected_digest,
        ]
    return None


def clear_guest_protection(run_id: str, expected_digest: str) -> str:
    facts = _facts(VM_ID)
    command = protection_update_args(facts, run_id, expected_digest)
    if command is not None:
        try:
            subprocess.run(
                ["ssh", "-o", "BatchMode=yes", "-o", "RequestTTY=no", "-o", "ConnectTimeout=5",
                 "pve", "sudo", "-n", *command],
                check=True, capture_output=True, text=True, timeout=30,
            )
        except (OSError, subprocess.SubprocessError) as exc:
            raise BoundaryError("could not clear protection on the identity-checked VM 9020") from exc
    verified = _facts(VM_ID)
    digest = check_guest(verified, run_id, allow_stopped=True)
    _require(verified.get("protection", 0) in (0, False, "0"), "VM 9020 protection remains enabled")
    return digest


def check_network_facts(facts: dict[str, Any], run_id: str) -> None:
    _require(RUN_RE.fullmatch(run_id) is not None, "run marker is not canonical")
    _require(facts.get("bridge") == BRIDGE, "active recipe bridge is not vmbr9")
    _require(facts.get("bridge_type") == "bridge", "active recipe interface is not a Linux bridge")
    _require(facts.get("address") == "10.250.9.1/24", "active recipe bridge address mismatch")
    _require(facts.get("saved_gateway") == "10.250.9.1", "saved recipe gateway mismatch")
    _require(facts.get("saved_subnet") == "10.250.9.0/24", "saved recipe subnet mismatch")
    _require(facts.get("run_id") == run_id, "active firewall belongs to another run")
    _require(facts.get("firewall_table") == f"ra8_lab_ci_{run_id}", "active firewall table mismatch")


def _ssh_json(path: str) -> dict[str, Any]:
    command = [
        "ssh", "-o", "BatchMode=yes", "-o", "RequestTTY=no", "-o", "ConnectTimeout=5",
        "pve", "sudo", "-n", "pvesh", "get", path, "--output-format", "json",
    ]
    try:
        result = subprocess.run(command, check=True, capture_output=True, text=True, timeout=20)
        value = json.loads(result.stdout)
    except (OSError, subprocess.SubprocessError, json.JSONDecodeError) as exc:
        raise BoundaryError("could not read allowlisted Proxmox facts over the pve SSH path") from exc
    if not isinstance(value, dict):
        raise BoundaryError("Proxmox returned an invalid configuration object")
    return value


def _facts(vmid: int) -> dict[str, Any]:
    try:
        resources_cmd = [
            "ssh", "-o", "BatchMode=yes", "-o", "RequestTTY=no", "-o", "ConnectTimeout=5",
            "pve", "sudo", "-n", "pvesh", "get", "/cluster/resources", "--type", "vm", "--output-format", "json",
        ]
        output = subprocess.run(resources_cmd, check=True, capture_output=True, text=True, timeout=20).stdout
        resources = json.loads(output)
    except (OSError, subprocess.SubprocessError, json.JSONDecodeError) as exc:
        raise BoundaryError("could not read the Proxmox VM inventory over the pve SSH path") from exc
    resource = next((row for row in resources if row.get("vmid") == vmid), None)
    if not isinstance(resource, dict):
        raise BoundaryError("Proxmox VM is absent from the inventory")
    config = _ssh_json(f"/nodes/{NODE}/qemu/{vmid}/config")
    disk_storage = {
        _storage_name(value)
        for key, value in config.items()
        if re.match(r"^(?:scsi|virtio|sata|ide|efidisk|tpmstate|unused)[0-9]+$", key)
        and isinstance(value, str) and ":" in value and "media=cdrom" not in value
    }
    net0 = str(config.get("net0", ""))
    bridge_match = re.search(r"(?:^|,)bridge=([^,]+)", net0)
    tags = config.get("tags", "")
    return {
        "vmid": vmid,
        "name": config.get("name", ""),
        "node": resource.get("node", ""),
        "pool": resource.get("pool", ""),
        "template": resource.get("template") in (1, True, "1"),
        "status": resource.get("status", ""),
        "description": config.get("description", ""),
        "tags": tags.split(";") if isinstance(tags, str) else list(tags or []),
        "bridge": bridge_match.group(1) if bridge_match else "",
        "datastore": next(iter(disk_storage)) if len(disk_storage) == 1 else "",
        "digest": config.get("digest", ""),
        "cores": config.get("cores", 0),
        "sockets": config.get("sockets", 1),
        "memory": config.get("memory", 0),
        "protection": config.get("protection", 0),
        "config": config,
    }


def _network_ok(run_id: str) -> None:
    _require(RUN_RE.fullmatch(run_id) is not None, "run marker is not canonical")
    remote = r'''set -euo pipefail
bridge="$1"
run_id="$2"
address="$(ip -o -4 addr show dev "$bridge" | awk 'NR == 1 {print $4}')"
[[ "$address" == "10.250.9.1/24" ]]
ip -d link show dev "$bridge" | grep -q 'bridge'
[[ -d /run/ra8-lab-ci && ! -L /run/ra8-lab-ci && "$(stat -c %u /run/ra8-lab-ci)" == 0 && "$(stat -c %a /run/ra8-lab-ci)" == 700 ]]
[[ -f /run/ra8-lab-ci/vmbr9.state && ! -L /run/ra8-lab-ci/vmbr9.state && "$(stat -c %u /run/ra8-lab-ci/vmbr9.state)" == 0 && "$(stat -c %a /run/ra8-lab-ci/vmbr9.state)" == 600 ]]
read -r old_forward saved_gateway saved_subnet </run/ra8-lab-ci/vmbr9.state
[[ ( "$old_forward" == 0 || "$old_forward" == 1 ) && "$saved_gateway" == "10.250.9.1" && "$saved_subnet" == "10.250.9.0/24" ]]
table="ra8_lab_ci_${run_id}"
nft list table ip "$table" >/dev/null
printf '{"bridge":"%s","bridge_type":"bridge","address":"%s","saved_gateway":"%s","saved_subnet":"%s","run_id":"%s","firewall_table":"%s"}\n' \
  "$bridge" "$address" "$saved_gateway" "$saved_subnet" "$run_id" "$table"
'''
    try:
        result = subprocess.run(
            ["ssh", "-o", "BatchMode=yes", "-o", "RequestTTY=no", "pve", "sudo", "-n", "/bin/bash", "-s", "--", BRIDGE, run_id],
            input=remote, check=True, capture_output=True, text=True, timeout=20,
        )
        facts = json.loads(result.stdout)
    except (OSError, subprocess.SubprocessError) as exc:
        raise BoundaryError("vmbr9 is not the active bridge owned by this recipe run") from exc
    except json.JSONDecodeError as exc:
        raise BoundaryError("the lab recipe returned invalid bridge evidence") from exc
    if not isinstance(facts, dict):
        raise BoundaryError("the lab recipe returned invalid bridge evidence")
    check_network_facts(facts, run_id)


def _expect_rejected(label: str, operation: Any) -> None:
    try:
        operation()
    except BoundaryError:
        return
    raise AssertionError(f"selftest: accepted invalid {label}")


def selftest() -> None:
    run_id = "0123456789abcdef"
    base = {
        "vmid": TEMPLATE_ID, "name": TEMPLATE_NAME, "template": True, "status": "stopped",
        "pool": POOL, "description": f"Debian template {TEMPLATE_MARKER}", "datastore": DATASTORE,
        "digest": "a" * 40,
    }
    assert check_template(base) == "a" * 40
    for label, field, value in (
        ("template ID", "vmid", 9002), ("template name", "name", "wrong-template"),
        ("template flag", "template", False), ("stopped template state", "status", "running"),
        ("template pool", "pool", "production"), ("template datastore", "datastore", "local-lvm"),
        ("template readiness marker", "description", "unreviewed"), ("template digest", "digest", "bad"),
    ):
        broken = dict(base)
        broken[field] = value
        _expect_rejected(label, lambda broken=broken: check_template(broken))

    network = {
        "bridge": BRIDGE, "bridge_type": "bridge", "address": "10.250.9.1/24",
        "saved_gateway": "10.250.9.1", "saved_subnet": "10.250.9.0/24",
        "run_id": run_id, "firewall_table": f"ra8_lab_ci_{run_id}",
    }
    check_network_facts(network, run_id)
    for label, field, value in (
        ("recipe bridge", "bridge", "vmbr0"), ("recipe run marker", "run_id", "fedcba9876543210"),
        ("recipe firewall table", "firewall_table", "ra8_lab_ci_other"),
        ("recipe bridge address", "address", "192.168.1.1/24"),
        ("recipe subnet marker", "saved_subnet", "192.168.1.0/24"),
    ):
        broken = dict(network)
        broken[field] = value
        _expect_rejected(label, lambda broken=broken: check_network_facts(broken, run_id))

    guest_config = {
        "scsi0": "ra8-tf-lab:vm-9020-disk-0,size=32G",
        "net0": "virtio=02:00:00:00:00:01,bridge=vmbr9,firewall=1",
    }
    guest = {
        "vmid": VM_ID, "name": f"ra8-lab-linux-{run_id}", "node": NODE, "pool": POOL,
        "template": False, "status": "running",
        "description": f"Disposable RA8 CI lifecycle guest; RA8_LAB_RUN={run_id}",
        "tags": [f"run-{run_id}"], "bridge": BRIDGE, "datastore": DATASTORE,
        "digest": "b" * 40, "cores": 4, "sockets": 1, "memory": 8192, "config": guest_config,
    }
    assert check_guest(guest, run_id) == "b" * 40
    stopped_guest = dict(guest, status="stopped")
    assert check_guest(stopped_guest, run_id, allow_stopped=True) == "b" * 40
    _expect_rejected("stopped guest during create", lambda: check_guest(stopped_guest, run_id))
    protected_guest = dict(guest, protection=1)
    update_args = protection_update_args(protected_guest, run_id, "b" * 40)
    assert update_args == [
        "pvesh", "set", "/nodes/pve1/qemu/9020/config", "--protection", "0", "--digest", "b" * 40,
    ]
    assert protection_update_args(dict(guest, protection=0), run_id, "b" * 40) is None
    _expect_rejected("protection update with wrong digest", lambda: protection_update_args(protected_guest, run_id, "c" * 40))
    for label, field, value in (
        ("VMID range", "vmid", 9100), ("assigned VMID", "vmid", 9005), ("run marker", "description", "other run"),
        ("guest bridge", "bridge", "vmbr0"), ("guest pool", "pool", "production"),
        ("guest datastore", "datastore", "local-lvm"), ("guest run tag", "tags", []),
        ("post-copy digest binding", "digest", "c" * 40), ("guest boot state", "status", "stopped"),
        ("guest CPU ceiling", "cores", 5), ("guest memory ceiling", "memory", 8193),
    ):
        broken = dict(guest)
        broken[field] = value
        _expect_rejected(label, lambda broken=broken: check_guest(broken, run_id, "b" * 40))
    oversized_disk = dict(guest)
    oversized_disk["config"] = {"scsi0": "ra8-tf-lab:vm-9020-disk-0,size=33G", "net0": guest_config["net0"]}
    _expect_rejected("guest disk ceiling", lambda: check_guest(oversized_disk, run_id, "b" * 40))
    check_static(VM_ID, run_id, BRIDGE, POOL, DATASTORE)
    for label, values in (
        ("VMID outside range", (9100, run_id, BRIDGE, POOL, DATASTORE)),
        ("invalid bridge", (VM_ID, run_id, "vmbr0", POOL, DATASTORE)),
        ("invalid pool", (VM_ID, run_id, BRIDGE, "production", DATASTORE)),
        ("invalid datastore", (VM_ID, run_id, BRIDGE, POOL, "local-lvm")),
        ("invalid run marker", (VM_ID, "bad", BRIDGE, POOL, DATASTORE)),
    ):
        _expect_rejected(label, lambda values=values: check_static(*values))
    print("lab-guest-policy.py --selftest: PASS")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("action", nargs="?", choices=("check-template", "digest-template", "check-network", "check-guest", "check-guest-destroy", "clear-protection", "check-absent"))
    parser.add_argument("run_id", nargs="?")
    parser.add_argument("digest", nargs="?")
    args = parser.parse_args()
    try:
        if args.selftest:
            selftest()
        elif args.action is None:
            parser.error("choose --selftest or a read-only check action")
        elif args.action == "check-template":
            check_template(_facts(TEMPLATE_ID))
            print("template boundary: PASS (9001, stopped, identity and digest verified)")
        elif args.action == "digest-template":
            print(check_template(_facts(TEMPLATE_ID)))
        elif args.action == "check-network":
            _network_ok(args.run_id or "")
            print("recipe bridge boundary: PASS (vmbr9 and this run's firewall table verified)")
        elif args.action == "check-guest":
            digest = check_guest(_facts(VM_ID), args.run_id or "", args.digest or None)
            print(digest)
        elif args.action == "check-guest-destroy":
            digest = check_guest(_facts(VM_ID), args.run_id or "", args.digest or None, allow_stopped=True)
            print(digest)
        elif args.action == "clear-protection":
            if not args.digest:
                raise BoundaryError("a bound guest config digest is required before clearing protection")
            print(clear_guest_protection(args.run_id or "", args.digest))
        elif args.action == "check-absent":
            try:
                _facts(VM_ID)
            except BoundaryError as exc:
                if "absent from the inventory" in str(exc):
                    print("guest absent: PASS")
                    return 0
                raise
            raise BoundaryError("VM 9020 still exists after OpenTofu destroy")
    except (BoundaryError, AssertionError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
