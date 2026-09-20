#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Manage and connect to disposable Proxmox lab VMs.

Supports discovering multiple active lab VMs, interactive or direct selection,
SSH execution through the pve tunnel, and teardown.
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import re
import subprocess
import sys
import tempfile
from typing import Any, Dict, List, Optional

SSH_ALIAS = "pve"
DEFAULT_USER = "terraform-lab"
VM_IP_MAP = {
    9000: "10.250.9.10",
    9010: "10.250.9.20",
}


def get_active_vms() -> List[Dict[str, Any]]:
    """Query Proxmox for all lab VMs in the 9000-9099 range."""
    cmd = [
        "ssh",
        "-o",
        "BatchMode=yes",
        "-o",
        "RequestTTY=no",
        SSH_ALIAS,
        "sudo -n qm list",
    ]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, check=True)
    except subprocess.CalledProcessError as err:
        sys.stderr.write(f"error: failed to query Proxmox VMs: {err.stderr}\n")
        return []

    vms: List[Dict[str, Any]] = []
    lines = proc.stdout.strip().splitlines()
    if not lines:
        return []

    for line in lines[1:]:
        parts = line.split()
        if len(parts) < 3:
            continue
        try:
            vmid = int(parts[0])
        except ValueError:
            continue

        if 9000 <= vmid < 9100:
            name = parts[1]
            status = parts[2]
            run_match = re.search(r"-([0-9a-f]{16})$", name)
            run_id = run_match.group(1) if run_match else ""
            if "template" in name or vmid in (9001, 9011):
                vm_type = "template"
            elif "windows" in name or vmid == 9010:
                vm_type = "windows"
            else:
                vm_type = "linux"
            vms.append({
                "vmid": vmid,
                "name": name,
                "type": vm_type,
                "status": status,
                "run_id": run_id,
                "ip": VM_IP_MAP.get(vmid, f"10.250.9.{vmid % 100}"),
            })

    return vms


def find_run_key(run_id: str) -> Optional[str]:
    """Find the private key matching a given run_id or the newest run key."""
    tmp_dirs = sorted(
        glob.glob(os.path.join(tempfile.gettempdir(), "ra8-lab-ci.*")),
        key=os.path.getmtime,
        reverse=True,
    )
    if not tmp_dirs:
        return None

    if run_id:
        for d in tmp_dirs:
            pub_key = os.path.join(d, "id_ed25519.pub")
            if os.path.isfile(pub_key):
                try:
                    with open(pub_key, "r", encoding="utf-8") as f:
                        if run_id in f.read():
                            priv_key = os.path.join(d, "id_ed25519")
                            if os.path.isfile(priv_key):
                                return priv_key
                except OSError:
                    continue

    # Fallback to the newest run key
    for d in tmp_dirs:
        priv_key = os.path.join(d, "id_ed25519")
        if os.path.isfile(priv_key):
            return priv_key

    return None


def cmd_list(args: argparse.Namespace) -> int:
    vms = get_active_vms()
    if not vms:
        print("No 9000-series lab VMs currently found on Proxmox.")
        return 0

    print(f"{'VMID':<6} {'TYPE':<8} {'STATUS':<10} {'IP':<15} {'RUN ID':<18} {'NAME'}")
    print("-" * 75)
    for vm in vms:
        print(
            f"{vm['vmid']:<6} {vm['type']:<8} {vm['status']:<10} "
            f"{vm['ip']:<15} {vm['run_id'] or 'unknown':<18} {vm['name']}"
        )
    return 0


def cmd_ssh(args: argparse.Namespace) -> int:
    all_vms = get_active_vms()
    running_vms = [v for v in all_vms if v["status"] == "running"]

    if not running_vms:
        sys.stderr.write("error: no running lab VMs found on Proxmox.\n")
        return 1

    target = args.target
    remaining_cmd = args.command

    selected_vm: Optional[Dict[str, Any]] = None

    if target:
        # Check if target matches VMID
        try:
            target_vmid = int(target)
            for v in running_vms:
                if v["vmid"] == target_vmid:
                    selected_vm = v
                    break
        except ValueError:
            pass

        # Check if target matches type or run_id
        if not selected_vm:
            target_lower = target.lower()
            for v in running_vms:
                if v["type"] == target_lower or (v["run_id"] and v["run_id"].startswith(target_lower)):
                    selected_vm = v
                    break

        # If target didn't match any VM, check if only 1 VM exists
        # In that case, target was actually the command (e.g. `just ssh htop`)
        if not selected_vm:
            if len(running_vms) == 1:
                selected_vm = running_vms[0]
                remaining_cmd = [target] + remaining_cmd
            else:
                sys.stderr.write(f"error: ambiguous or unknown VM target '{target}'.\n")
                sys.stderr.write("Available VMs:\n")
                for v in running_vms:
                    sys.stderr.write(f"  - VM {v['vmid']} ({v['type']}, run {v['run_id']})\n")
                return 1

    if not selected_vm:
        if len(running_vms) == 1:
            selected_vm = running_vms[0]
        else:
            print("Multiple running lab VMs detected:")
            for idx, v in enumerate(running_vms, 1):
                print(f"  [{idx}] VM {v['vmid']} ({v['type']}) - IP: {v['ip']} - Run: {v['run_id']}")

            if sys.stdin.isatty():
                try:
                    choice = input(f"Select VM [1-{len(running_vms)}] (default 1): ").strip()
                    idx = int(choice) if choice else 1
                    if 1 <= idx <= len(running_vms):
                        selected_vm = running_vms[idx - 1]
                    else:
                        sys.stderr.write("error: invalid selection\n")
                        return 1
                except (ValueError, EOFError, KeyboardInterrupt):
                    sys.stderr.write("\nSelection aborted.\n")
                    return 1
            else:
                selected_vm = running_vms[0]

    key_path = find_run_key(selected_vm["run_id"])
    if not key_path:
        sys.stderr.write(
            f"error: could not find an SSH key for run '{selected_vm['run_id']}' in {tempfile.gettempdir()}.\n"
        )
        return 1

    ip = selected_vm["ip"]
    vmid = selected_vm["vmid"]
    print(f"Connecting to VM {vmid} ({selected_vm['type']} @ {ip})...", file=sys.stderr)

    ssh_args = [
        "ssh",
        "-i",
        key_path,
        "-o",
        "IdentitiesOnly=yes",
        "-o",
        "StrictHostKeyChecking=no",
        "-o",
        "UserKnownHostsFile=/dev/null",
        "-o",
        "LogLevel=ERROR",
        "-o",
        f"ProxyCommand=ssh -o BatchMode=yes {SSH_ALIAS} nc {ip} 22",
        f"{DEFAULT_USER}@{ip}",
    ]
    if remaining_cmd:
        ssh_args.extend(remaining_cmd)

    os.execvp("ssh", ssh_args)
    return 0


def cmd_destroy(args: argparse.Namespace) -> int:
    target = args.target
    all_vms = get_active_vms()
    target_vms: List[Dict[str, Any]] = []

    if target in ("", "all"):
        target_vms = [v for v in all_vms if v["type"] != "template"]
    else:
        try:
            target_vmid = int(target)
            target_vms = [v for v in all_vms if v["vmid"] == target_vmid and v["type"] != "template"]
        except ValueError:
            target_lower = target.lower()
            target_vms = [
                v for v in all_vms
                if v["type"] != "template" and (
                    v["type"] == target_lower or (v["run_id"] and v["run_id"].startswith(target_lower))
                )
            ]

    if not target_vms:
        print(f"No matching lab VMs found for target '{target}'. Cleaning network and temporary files.")
    else:
        for vm in target_vms:
            vmid = vm["vmid"]
            print(f"Tearing down VM {vmid} ({vm['name']})...")
            remote_cmd = f"""
            set -euo pipefail
            if qm status {vmid} >/dev/null 2>&1; then
                qm stop {vmid} >/dev/null 2>&1 || true
                for _ in {{1..30}}; do
                    [[ "$(qm status {vmid} | awk '{{print $2}}')" == "stopped" ]] && break
                    sleep 1
                done
                qm set {vmid} --protection 0 >/dev/null 2>&1 || true
                qm destroy {vmid} --purge 1 >/dev/null 2>&1 || true
            fi
            """
            subprocess.run(
                ["ssh", "-o", "BatchMode=yes", "-o", "RequestTTY=no", SSH_ALIAS, "sudo -n /bin/bash -s"],
                input=remote_cmd,
                text=True,
                check=False,
            )

    # Clean up network if all lab VMs destroyed
    remaining = [v for v in get_active_vms() if v["type"] != "template" and v["vmid"] not in [t["vmid"] for t in target_vms]]
    if not remaining:
        print("Cleaning up lab bridges (vmbr8, vmbr9), nftables, and host routes...")
        net_cleanup = """
        set -euo pipefail
        for br in vmbr8 vmbr9; do
            if ip link show "$br" >/dev/null 2>&1; then
                ip addr flush dev "$br" scope global >/dev/null 2>&1 || true
                ip link delete "$br" type bridge >/dev/null 2>&1 || true
            fi
        done
        for table in $(nft list tables 2>/dev/null | awk '$1 == "table" && $3 ~ /^ra8_lab_ci_/ {print $3}'); do
            nft delete table ip "$table" >/dev/null 2>&1 || true
        done
        sysctl -w net.ipv4.ip_forward=0 >/dev/null 2>&1 || true
        """
        subprocess.run(
            ["ssh", "-o", "BatchMode=yes", "-o", "RequestTTY=no", SSH_ALIAS, "sudo -n /bin/bash -s"],
            input=net_cleanup,
            text=True,
            check=False,
        )

    # Clean matching local run dirs
    for d in glob.glob(os.path.join(tempfile.gettempdir(), "ra8-lab-ci.*")):
        try:
            subprocess.run(["rm", "-rf", d], check=False)
        except OSError:
            pass

    print("Cleanup complete.")
    return 0


def main() -> None:
    if len(sys.argv) > 1 and sys.argv[1] == "ssh":
        target = sys.argv[2] if len(sys.argv) > 2 else ""
        command = sys.argv[3:] if len(sys.argv) > 3 else []
        args = argparse.Namespace(subcommand="ssh", target=target, command=command)
        sys.exit(cmd_ssh(args))

    parser = argparse.ArgumentParser(description="Proxmox Lab VM Management")
    subparsers = parser.add_subparsers(dest="subcommand", required=True)

    subparsers.add_parser("list", help="List active or preserved lab VMs")

    destroy_parser = subparsers.add_parser("destroy", help="Tear down preserved lab VMs and network")
    destroy_parser.add_argument("target", nargs="?", default="all", help="VM ID, type, or 'all'")

    args = parser.parse_args()
    if args.subcommand == "list":
        sys.exit(cmd_list(args))
    elif args.subcommand == "ssh":
        sys.exit(cmd_ssh(args))
    elif args.subcommand == "destroy":
        sys.exit(cmd_destroy(args))


if __name__ == "__main__":
    main()
