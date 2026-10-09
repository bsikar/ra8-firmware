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
PROFILES = {
    "linux": {
        "template_id": 9001,
        "template_name": "ra8-lab-debian-template",
        "template_marker": "RA8_LAB_TEMPLATE=linux-ci-v1",
        "vm_id": None,
        "vmid_min": 9020,
        "vmid_max": 9039,
        "reserved_vmids": (9021,),
        "guest_name": "ra8-lab-linux",
        "disk_limit_mib": 32 * 1024,
    },
    "windows": {
        "template_id": 9012,
        "template_name": "ra8-lab-windows-template",
        "template_marker": "RA8_LAB_TEMPLATE=windows-ci-v1",
        "vm_id": 9021,
        "vmid_min": 9021,
        "vmid_max": 9021,
        "reserved_vmids": (),
        "guest_name": "ra8-lab-windows",
        "disk_limit_mib": 64 * 1024,
    },
}
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
        if not isinstance(raw, str) or "media=cdrom" in raw:
            continue
        _require(_storage_name(raw) == DATASTORE, "disk is outside the allowlisted datastore")
        match = DISK_RE.search(raw)
        if match is None:
            continue
        size, unit = int(match.group(1)), match.group(2)
        total += size * {"K": 1 / 1024, "M": 1, "G": 1024}[unit]
    return int(total)


def _known_windows_clone_disk_overage(config: dict[str, Any]) -> bool:
    """Recognize only the historical Windows clone's two expected 64 GiB disks.

    This is accepted solely on the destroy and protection-clear path so a
    misconfigured clone can be removed. Provisioning remains capped at 64 GiB.
    """
    disk_keys = {
        key for key, raw in config.items()
        if re.match(r"^(?:scsi|virtio|sata|ide|efidisk|tpmstate|unused)[0-9]+$", key)
        and isinstance(raw, str) and "media=cdrom" not in raw
    }
    return (
        disk_keys == {"sata0", "scsi0"}
        and all(_disk_mib({key: config[key]}) == 64 * 1024 for key in disk_keys)
    )


def profile_facts(profile: str, vmid: int | None = None) -> dict[str, Any]:
    _require(profile in PROFILES, "guest profile is not allowlisted")
    selected = PROFILES[profile]
    if vmid is not None:
        _require(selected["vmid_min"] <= vmid <= selected["vmid_max"], "VMID is outside this profile's reserved range")
        _require(vmid not in selected["reserved_vmids"], "VMID is reserved for another guest profile")
        _require(selected["vm_id"] is None or vmid == selected["vm_id"], "VMID is not assigned to this lifecycle profile")
    return selected


def first_available_linux_vmid(occupied: set[int]) -> int:
    selected = PROFILES["linux"]
    for vmid in range(selected["vmid_min"], selected["vmid_max"] + 1):
        if vmid in selected["reserved_vmids"] or vmid in occupied:
            continue
        profile_facts("linux", vmid)
        return vmid
    raise BoundaryError("no unoccupied Linux reservation VMID is available")


def require_unoccupied(vmid: int, occupied: bool) -> None:
    _require(not occupied, f"VMID {vmid} is already occupied")


def check_static(profile: str, vmid: int, run_id: str, bridge: str, pool: str, datastore: str) -> None:
    selected = profile_facts(profile, vmid)
    _require(RUN_RE.fullmatch(run_id) is not None, "run marker is not canonical")
    _require(bridge == BRIDGE, "guest bridge is not the recipe's vmbr9")
    _require(pool == POOL, "guest pool is not allowlisted")
    _require(datastore == DATASTORE, "guest datastore is not allowlisted")


def check_template(facts: dict[str, Any], profile: str) -> str:
    selected = profile_facts(profile)
    _require(facts.get("vmid") == selected["template_id"], "template ID mismatch")
    _require(facts.get("name") == selected["template_name"], "template name mismatch")
    _require(facts.get("template") is True, "clone source is not a template")
    _require(facts.get("status") == "stopped", "clone source is not stopped")
    _require(facts.get("pool") == POOL, "template is outside the allowlisted pool")
    _require(selected["template_marker"] in str(facts.get("description", "")), "template readiness marker is missing")
    _require(facts.get("datastore") == DATASTORE, "template disk is outside the allowlisted datastore")
    digest = str(facts.get("digest", ""))
    _require(DIGEST_RE.fullmatch(digest) is not None, "template config digest is invalid")
    return digest


def check_guest(
    facts: dict[str, Any], run_id: str, expected_digest: str | None = None, *,
    profile: str = "linux", allow_stopped: bool = False,
    cleanup_recovery: bool = False,
) -> str:
    selected = profile_facts(profile)
    vmid = int(facts.get("vmid", 0))
    check_static(profile, vmid, run_id, str(facts.get("bridge", "")), str(facts.get("pool", "")), str(facts.get("datastore", "")))
    _require(facts.get("node") == NODE, "guest is on an unapproved node")
    _require(facts.get("name") == f"{selected['guest_name']}-{run_id}", "guest name does not match this run")
    _require(facts.get("template") is False, "target VM is a template")
    _require(facts.get("description") == f"Disposable RA8 CI lifecycle guest; RA8_LAB_RUN={run_id}", "guest run marker mismatch")
    _require(f"run-{run_id}" in facts.get("tags", []), "guest run tag mismatch")
    statuses = ("running", "stopped") if allow_stopped else ("running",)
    _require(facts.get("status") in statuses, "guest is not in an allowed lifecycle state")
    _require(int(facts.get("cores", 0)) * int(facts.get("sockets", 1)) <= 4, "guest exceeds 4 vCPU")
    _require(int(facts.get("memory", 0)) <= 8192, "guest exceeds 8192 MiB memory")
    config = facts.get("config", {})
    disk_mib = _disk_mib(config)
    within_disk_ceiling = disk_mib <= selected["disk_limit_mib"]
    known_cleanup_overage = (
        cleanup_recovery and profile == "windows" and
        disk_mib == 2 * selected["disk_limit_mib"] and
        _known_windows_clone_disk_overage(config)
    )
    _require(within_disk_ceiling or known_cleanup_overage, "guest exceeds its profile disk ceiling")
    digest = str(facts.get("digest", ""))
    _require(DIGEST_RE.fullmatch(digest) is not None, "guest config digest is invalid")
    if expected_digest is not None:
        _require(digest == expected_digest, "guest config digest changed after it was bound")
    return digest


def protection_update_args(
    facts: dict[str, Any], run_id: str, expected_digest: str, profile: str,
    *, cleanup_recovery: bool = False,
) -> list[str] | None:
    _require(DIGEST_RE.fullmatch(expected_digest) is not None, "bound guest config digest is invalid")
    check_guest(facts, run_id, expected_digest, profile=profile, allow_stopped=True,
                cleanup_recovery=cleanup_recovery)
    protection = facts.get("protection", 0)
    _require(protection in (0, 1, False, True, "0", "1"), "guest protection state is invalid")
    if protection in (1, True, "1"):
        return [
            "pvesh", "set", f"/nodes/{NODE}/qemu/{facts['vmid']}/config",
            "--protection", "0", "--digest", expected_digest,
        ]
    return None


def _guest_vmid(profile: str, vmid: int | None) -> int:
    selected = profile_facts(profile, vmid)
    resolved = selected["vm_id"] if vmid is None else vmid
    _require(resolved is not None, "an explicit VMID is required for this profile action")
    return int(resolved)


def clear_guest_protection(run_id: str, expected_digest: str, profile: str, vmid: int) -> str:
    profile_facts(profile, vmid)
    facts = _facts(vmid)
    command = protection_update_args(facts, run_id, expected_digest, profile,
                                     cleanup_recovery=True)
    if command is not None:
        try:
            subprocess.run(
                ["ssh", "-o", "BatchMode=yes", "-o", "RequestTTY=no", "-o", "ConnectTimeout=5",
                 "pve", "sudo", "-n", *command],
                check=True, capture_output=True, text=True, timeout=30,
            )
        except (OSError, subprocess.SubprocessError) as exc:
            raise BoundaryError("could not clear protection on the identity-checked lifecycle guest") from exc
    verified = _facts(vmid)
    digest = check_guest(verified, run_id, profile=profile, allow_stopped=True,
                         cleanup_recovery=True)
    _require(verified.get("protection", 0) in (0, False, "0"), "guest protection remains enabled")
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


def _is_absent(vmid: int) -> bool:
    try:
        _facts(vmid)
    except BoundaryError as exc:
        if "absent from the inventory" in str(exc):
            return True
        raise
    return False


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
    for profile, selected in PROFILES.items():
        base = {
            "vmid": selected["template_id"], "name": selected["template_name"],
            "template": True, "status": "stopped", "pool": POOL,
            "description": f"Windows template {selected['template_marker']}",
            "datastore": DATASTORE, "digest": "a" * 40,
        }
        assert check_template(base, profile) == "a" * 40
        for label, field, value in (
            ("template ID", "vmid", 9002), ("template name", "name", "wrong-template"),
            ("template flag", "template", False), ("stopped template state", "status", "running"),
            ("template pool", "pool", "production"), ("template datastore", "datastore", "local-lvm"),
            ("template readiness marker", "description", "unreviewed"), ("template digest", "digest", "bad"),
        ):
            broken = dict(base)
            broken[field] = value
            _expect_rejected(f"{profile} {label}", lambda broken=broken, profile=profile: check_template(broken, profile))

    _expect_rejected("unknown profile", lambda: profile_facts("other"))

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

    for profile, selected in PROFILES.items():
        vmid = 9020 if profile == "linux" else int(selected["vm_id"])
        disk_gib = selected["disk_limit_mib"] // 1024
        disk_interface = "sata0" if profile == "windows" else "scsi0"
        guest_config = {
            disk_interface: f"ra8-tf-lab:vm-{vmid}-disk-0,size={disk_gib}G",
            "ide2": "ra8-tf-lab:vm-guest-cloudinit,media=cdrom",
            "net0": "virtio=02:00:00:00:00:01,bridge=vmbr9,firewall=1",
        }
        guest = {
            "vmid": vmid, "name": f"{selected['guest_name']}-{run_id}", "node": NODE, "pool": POOL,
            "template": False, "status": "running",
            "description": f"Disposable RA8 CI lifecycle guest; RA8_LAB_RUN={run_id}",
            "tags": [f"run-{run_id}"], "bridge": BRIDGE, "datastore": DATASTORE,
            "digest": "b" * 40, "cores": 4, "sockets": 1, "memory": 8192, "config": guest_config,
        }
        assert check_guest(guest, run_id, profile=profile) == "b" * 40
        if profile == "windows":
            duplicate_disk_guest = dict(guest)
            duplicate_disk_guest["config"] = dict(guest_config, scsi0="ra8-tf-lab:vm-9021-disk-1,size=64G")
            _expect_rejected("Windows duplicate cloned disk during provisioning", lambda: check_guest(duplicate_disk_guest, run_id, profile="windows"))
            assert check_guest(duplicate_disk_guest, run_id, profile="windows", allow_stopped=True,
                               cleanup_recovery=True) == "b" * 40
            for broken_key, broken_value, label in (
                ("virtio0", "ra8-tf-lab:extra,size=64G", "extra third Windows disk during cleanup"),
                ("scsi0", "other-store:vm-9021-disk-1,size=64G", "Windows cleanup disk outside datastore"),
                ("scsi0", "ra8-tf-lab:vm-9021-disk-1,size=65G", "oversized Windows cleanup disk"),
            ):
                bad = dict(duplicate_disk_guest)
                bad["config"] = dict(duplicate_disk_guest["config"], **{broken_key: broken_value})
                _expect_rejected(label, lambda bad=bad: check_guest(bad, run_id, profile="windows",
                                                                      allow_stopped=True, cleanup_recovery=True))
        stopped_guest = dict(guest, status="stopped")
        assert check_guest(stopped_guest, run_id, profile=profile, allow_stopped=True) == "b" * 40
        _expect_rejected(f"{profile} stopped guest during create", lambda: check_guest(stopped_guest, run_id, profile=profile))
        protected_guest = dict(guest, protection=1)
        update_args = protection_update_args(protected_guest, run_id, "b" * 40, profile)
        assert update_args == [
            "pvesh", "set", f"/nodes/pve1/qemu/{vmid}/config", "--protection", "0", "--digest", "b" * 40,
        ]
        assert protection_update_args(dict(guest, protection=0), run_id, "b" * 40, profile) is None
        _expect_rejected(f"{profile} protection update with wrong digest", lambda: protection_update_args(protected_guest, run_id, "c" * 40, profile))
        for label, field, value in (
            ("VMID range", "vmid", 9100), ("assigned VMID", "vmid", 9005), ("run marker", "description", "other run"),
            ("guest bridge", "bridge", "vmbr0"), ("guest pool", "pool", "production"),
            ("guest datastore", "datastore", "local-lvm"), ("guest run tag", "tags", []),
            ("post-copy digest binding", "digest", "c" * 40), ("guest boot state", "status", "stopped"),
            ("guest CPU ceiling", "cores", 5), ("guest memory ceiling", "memory", 8193),
        ):
            broken = dict(guest)
            broken[field] = value
            _expect_rejected(f"{profile} {label}", lambda broken=broken, profile=profile: check_guest(broken, run_id, "b" * 40, profile=profile))
        oversized_disk = dict(guest)
        oversized_disk["config"] = {"scsi0": f"ra8-tf-lab:vm-{vmid}-disk-0,size={disk_gib + 1}G", "net0": guest_config["net0"]}
        _expect_rejected(f"{profile} guest disk ceiling", lambda: check_guest(oversized_disk, run_id, profile=profile))
        other_vmid = 9040 if profile == "linux" else 9020
        _expect_rejected(f"{profile} VMID/profile mismatch", lambda: check_guest(dict(guest, vmid=other_vmid), run_id, profile=profile))
    _expect_rejected("Linux profile VMID reserved for Windows", lambda: profile_facts("linux", 9021))
    _expect_rejected(
        "Linux static check rejects Windows VMID",
        lambda: check_static("linux", 9021, run_id, BRIDGE, POOL, DATASTORE),
    )
    check_static("linux", 9039, run_id, BRIDGE, POOL, DATASTORE)
    assert first_available_linux_vmid(set()) == 9020
    assert first_available_linux_vmid({9020}) == 9022
    assert first_available_linux_vmid({9020, 9021, 9022}) == 9023
    _expect_rejected("exhausted Linux reservation range", lambda: first_available_linux_vmid(set(range(9020, 9040))))
    require_unoccupied(9021, False)
    _expect_rejected("occupied Windows VMID", lambda: require_unoccupied(9021, True))
    check_static("linux", 9020, run_id, BRIDGE, POOL, DATASTORE)
    check_static("linux", 9022, run_id, BRIDGE, POOL, DATASTORE)
    check_static("windows", 9021, run_id, BRIDGE, POOL, DATASTORE)
    for label, values in (
        ("VMID outside range", ("windows", 9100, run_id, BRIDGE, POOL, DATASTORE)),
        ("invalid bridge", ("windows", 9021, run_id, "vmbr0", POOL, DATASTORE)),
        ("invalid pool", ("windows", 9021, run_id, BRIDGE, "production", DATASTORE)),
        ("invalid datastore", ("windows", 9021, run_id, BRIDGE, POOL, "local-lvm")),
        ("invalid run marker", ("windows", 9021, "bad", BRIDGE, POOL, DATASTORE)),
    ):
        _expect_rejected(label, lambda values=values: check_static(*values))
    print("lab-guest-policy.py --selftest: PASS")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--profile", choices=sorted(PROFILES), default="linux")
    parser.add_argument("--vm-id", type=int)
    parser.add_argument("action", nargs="?", choices=("find-free", "check-template", "digest-template", "check-network", "check-guest", "check-guest-destroy", "clear-protection", "check-absent"))
    parser.add_argument("run_id", nargs="?")
    parser.add_argument("digest", nargs="?")
    args = parser.parse_args()
    try:
        if args.selftest:
            selftest()
        elif args.action is None:
            parser.error("choose --selftest or a read-only check action")
        elif args.action == "find-free":
            profile_facts(args.profile)
            if args.profile != "linux":
                raise BoundaryError("automatic VMID allocation is available only for Linux reservations")
            occupied: set[int] = set()
            for candidate in range(PROFILES["linux"]["vmid_min"], PROFILES["linux"]["vmid_max"] + 1):
                if candidate in PROFILES["linux"]["reserved_vmids"]:
                    continue
                try:
                    _facts(candidate)
                except BoundaryError as exc:
                    if "absent from the inventory" in str(exc):
                        continue
                    raise
                occupied.add(candidate)
            print(first_available_linux_vmid(occupied))
        elif args.action == "check-template":
            selected = profile_facts(args.profile)
            check_template(_facts(selected["template_id"]), args.profile)
            print(f"template boundary: PASS ({selected['template_id']}, stopped, identity and digest verified)")
        elif args.action == "digest-template":
            selected = profile_facts(args.profile)
            print(check_template(_facts(selected["template_id"]), args.profile))
        elif args.action == "check-network":
            _network_ok(args.run_id or "")
            print("recipe bridge boundary: PASS (vmbr9 and this run's firewall table verified)")
        elif args.action == "check-guest":
            vmid = _guest_vmid(args.profile, args.vm_id)
            digest = check_guest(_facts(vmid), args.run_id or "", args.digest or None, profile=args.profile)
            print(digest)
        elif args.action == "check-guest-destroy":
            vmid = _guest_vmid(args.profile, args.vm_id)
            digest = check_guest(_facts(vmid), args.run_id or "", args.digest or None,
                                 profile=args.profile, allow_stopped=True, cleanup_recovery=True)
            print(digest)
        elif args.action == "clear-protection":
            if not args.digest:
                raise BoundaryError("a bound guest config digest is required before clearing protection")
            vmid = _guest_vmid(args.profile, args.vm_id)
            print(clear_guest_protection(args.run_id or "", args.digest, args.profile, vmid))
        elif args.action == "check-absent":
            vmid = _guest_vmid(args.profile, args.vm_id)
            require_unoccupied(vmid, not _is_absent(vmid))
            print("guest absent: PASS")
    except (BoundaryError, AssertionError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
