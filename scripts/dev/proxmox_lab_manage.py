#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Manage, run, stream logs, and connect to disposable Proxmox lab VMs.

Supports running detached server-side CI, live log streaming (with non-terminating
Ctrl+C detachment), multi-machine observation, interactive/direct SSH selection,
and comprehensive teardown.
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile
from typing import Any

SSH_ALIAS = "pve"
DEFAULT_USER = "terraform-lab"
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))

VM_IP_MAP = {
    9000: "10.250.9.10",
    9010: "10.250.8.20",
}


def get_active_vms() -> list[dict[str, Any]]:
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

    vms: list[dict[str, Any]] = []
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


def find_run_key(run_id: str) -> str | None:
    """Find the private key matching a given run_id or the newest run key."""
    tmp_dirs = sorted(
        glob.glob(os.path.join(tempfile.gettempdir(), "ra8-lab-ci.*")),
        key=os.path.getmtime,
        reverse=True,
    )
    if run_id:
        for d in tmp_dirs:
            pub_key = os.path.join(d, "id_ed25519.pub")
            if os.path.isfile(pub_key):
                try:
                    with open(pub_key, encoding="utf-8") as f:
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


def build_source_archive(output_path: str) -> int:
    """Package working tree including tracked, submodules, and untracked files into tarball."""
    temp_dir = tempfile.mkdtemp(prefix="ra8-lab-src-")
    stage_dir = os.path.join(temp_dir, "stage")
    os.makedirs(stage_dir, exist_ok=True)
    try:
        # 1. Archive committed HEAD
        p1 = subprocess.Popen(["git", "-C", REPO_ROOT, "archive", "--format=tar", "HEAD"], stdout=subprocess.PIPE)
        subprocess.run(["tar", "-xpf", "-", "-C", stage_dir], stdin=p1.stdout, check=True)
        if p1.stdout:
            p1.stdout.close()
        p1.wait()

        # 2. Submodules
        sub_cmd = "git submodule foreach --recursive 'mkdir -p \"$stage_dir/$path\" && git archive --format=tar HEAD | tar -xpf - -C \"$stage_dir/$path\"'"
        env = dict(os.environ, stage_dir=stage_dir)
        subprocess.run(["bash", "-c", sub_cmd], cwd=REPO_ROOT, env=env, check=False)

        # 3. Apply unstaged git diff
        diff = subprocess.run(["git", "-C", REPO_ROOT, "diff", "--no-ext-diff", "--binary", "HEAD"], capture_output=True)
        if diff.stdout:
            p = subprocess.Popen(["git", "-C", stage_dir, "apply", "--binary", "--whitespace=nowarn"], stdin=subprocess.PIPE)
            p.communicate(input=diff.stdout)

        # 4. Copy untracked files
        untracked = subprocess.run(
            ["git", "-C", REPO_ROOT, "ls-files", "--others", "--exclude-standard", "-z"],
            capture_output=True,
            check=True,
        )
        for raw_path in untracked.stdout.split(b"\0"):
            if not raw_path:
                continue
            path = raw_path.decode("utf-8", errors="replace")
            src_file = os.path.join(REPO_ROOT, path)
            dst_file = os.path.join(stage_dir, path)
            os.makedirs(os.path.dirname(dst_file), exist_ok=True)
            if os.path.isfile(src_file):
                shutil.copy2(src_file, dst_file)

        # 5. Tar the staged directory into output_path
        subprocess.run(
            ["tar", "--format=ustar", "--exclude=._*", "-cf", output_path, "-C", stage_dir, "."],
            env=dict(os.environ, COPYFILE_DISABLE="1"),
            check=True,
        )
        return os.path.getsize(output_path)
    finally:
        shutil.rmtree(temp_dir, ignore_errors=True)


def cmd_start(args: argparse.Namespace) -> int:
    """Start CI execution asynchronously on the Proxmox server."""
    profile = args.profile.lower()
    if profile not in ("linux", "windows"):
        sys.stderr.write(f"error: profile must be 'linux' or 'windows', got '{profile}'\n")
        return 1

    # Preflight check: is this profile currently active on the server?
    check_cmd = (
        f"sudo test -f /var/log/ra8-lab/{profile}.pid && "
        f"sudo kill -0 $(cat /var/log/ra8-lab/{profile}.pid 2>/dev/null) 2>/dev/null && "
        f"echo BUSY || echo IDLE"
    )
    res = subprocess.run(["ssh", "-o", "BatchMode=yes", SSH_ALIAS, check_cmd], capture_output=True, text=True)
    if "BUSY" in res.stdout:
        sys.stderr.write(f"error: a {profile} CI job is already actively running on {SSH_ALIAS}!\n")
        sys.stderr.write(f"  • View live logs:  just infra::lab::logs {profile}\n")
        sys.stderr.write(f"  • Stop running job: just infra::lab::stop {profile}\n")
        return 1

    run_id = os.urandom(8).hex()
    tar_path = os.path.join(tempfile.gettempdir(), f"ra8-lab-{profile}-{run_id}.tar")

    print(f"==> Packaging local workspace for {profile} CI (including uncommitted changes)...")
    size_bytes = build_source_archive(tar_path)
    print(f"    Payload packaged: {size_bytes / (1024 * 1024):.1f} MB")

    print(f"==> Uploading payload and runner to Proxmox host ({SSH_ALIAS})...")
    remote_dir = f"/var/lib/ra8-lab/{profile}"
    runner_script = os.path.join(SCRIPT_DIR, "proxmox_lab_server_runner.sh")

    setup_cmd = "sudo mkdir -p /var/lib/ra8-lab/linux /var/lib/ra8-lab/windows /var/log/ra8-lab && sudo chown -R $(whoami) /var/lib/ra8-lab /var/log/ra8-lab"
    subprocess.run(["ssh", "-o", "BatchMode=yes", SSH_ALIAS, setup_cmd], check=True)

    # PVE's restricted SSH service intermittently stalls the SFTP-backed
    # scp implementation during large uploads. Use the legacy SCP protocol
    # for these two controller-to-host transfers instead.
    subprocess.run(["scp", "-O", "-q", tar_path, f"{SSH_ALIAS}:{remote_dir}/source.tar"], check=True)
    subprocess.run(["scp", "-O", "-q", runner_script, f"{SSH_ALIAS}:/var/lib/ra8-lab/runner.sh"], check=True)
    try:
        os.remove(tar_path)
    except OSError:
        pass

    keep_str = "true" if getattr(args, "keep", False) else "false"
    launch_cmd = (
        "sudo -n chmod +x /var/lib/ra8-lab/runner.sh && "
        f"(sudo -n nohup /bin/bash /var/lib/ra8-lab/runner.sh {profile} {run_id} "
        f"{remote_dir}/source.tar {keep_str} </dev/null >/dev/null 2>&1 &)"
    )
    subprocess.run(["ssh", "-o", "BatchMode=yes", SSH_ALIAS, launch_cmd], check=True)

    print(f"==> {profile.capitalize()} CI run started on {SSH_ALIAS} (run_id: {run_id})")
    print(f"    • Stream live logs:    just infra::lab::logs {profile}")
    print("    • Check status:        just infra::lab::status")
    print(f"    • Stop/cancel:         just infra::lab::stop {profile}")
    return 0


def cmd_logs(args: argparse.Namespace) -> int:
    """Stream live CI logs from the Proxmox server with safe non-killing Ctrl+C detach."""
    profile = getattr(args, "profile", "linux").lower()
    log_file = f"/var/log/ra8-lab/{profile}.log"
    print(f"==> Attaching to {profile} CI log stream on {SSH_ALIAS}...")
    print("    (Press Ctrl+C at any time to detach without stopping the CI run)\n")

    tail_cmd = [
        "ssh",
        "-t",
        "-o", "LogLevel=ERROR",
        SSH_ALIAS,
        f"sudo test -f {log_file} || {{ echo 'No log file found at {log_file}. Run has not started yet.'; exit 1; }}; sudo tail -n 50 -f {log_file}"
    ]
    try:
        proc = subprocess.run(tail_cmd)
        return proc.returncode
    except KeyboardInterrupt:
        print("\n^C\n==> Detached from log stream.")
        print("    The CI run is still executing on the server!")
        print(f"    • Re-attach logs:  just infra::lab::logs {profile}")
        print("    • Check status:   just infra::lab::status")
        print(f"    • Stop/cancel:    just infra::lab::stop {profile}")
        return 0


def cmd_ci(args: argparse.Namespace) -> int:
    """Start CI execution on Proxmox and automatically attach to live logs."""
    rc = cmd_start(args)
    if rc != 0:
        return rc
    return cmd_logs(args)


def cmd_status(args: argparse.Namespace) -> int:
    """Show current status of background CI jobs on Proxmox."""
    print(f"=== Proxmox Lab CI Status ({SSH_ALIAS}) ===")
    status_script = """
    for profile in linux windows; do
      pid_file="/var/log/ra8-lab/${profile}.pid"
      status_file="/var/log/ra8-lab/${profile}.status"
      log_file="/var/log/ra8-lab/${profile}.log"
      if [[ -f "$pid_file" ]] && kill -0 "$(cat "$pid_file" 2>/dev/null)" 2>/dev/null; then
        pid=$(cat "$pid_file")
        echo "${profile^^}: RUNNING (PID: $pid)"
        if [[ -f "$log_file" ]]; then
          echo "  Last output: $(tail -n 1 "$log_file")"
        fi
      elif [[ -f "$status_file" ]]; then
        st=$(cat "$status_file")
        echo "${profile^^}: COMPLETED (${st})"
        if [[ -f "$log_file" ]]; then
          echo "  Final message: $(tail -n 1 "$log_file")"
        fi
      else
        echo "${profile^^}: IDLE (no active run)"
      fi
    done
    """
    subprocess.run(["ssh", "-o", "BatchMode=yes", SSH_ALIAS, f"sudo bash -c '{status_script}'"])
    print()
    cmd_list(args)
    return 0


def cmd_stop(args: argparse.Namespace) -> int:
    """Stop running CI job(s) on Proxmox and tear down the environment."""
    target = getattr(args, "profile", "all").lower()
    profiles = ["linux", "windows"] if target in ("all", "") else [target]

    for profile in profiles:
        print(f"==> Stopping {profile} CI background runner on {SSH_ALIAS}...")
        stop_script = f"""
        pid_file="/var/log/ra8-lab/{profile}.pid"
        status_file="/var/log/ra8-lab/{profile}.status"
        if [[ -f "$pid_file" ]]; then
          pid=$(cat "$pid_file" 2>/dev/null)
          if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
            for _ in {{1..15}}; do
              kill -0 "$pid" 2>/dev/null || break
              sleep 1
            done
            kill -9 "$pid" 2>/dev/null || true
          fi
          rm -f "$pid_file"
        fi
        if [[ -f "$status_file" ]] && [[ "$(cat "$status_file" 2>/dev/null)" == "RUNNING" ]]; then
          printf 'FAILED\\n' > "$status_file"
        fi
        """
        subprocess.run(["ssh", "-o", "BatchMode=yes", SSH_ALIAS, f"sudo bash -c '{stop_script}'"])

    # Destroy only the requested profile's VM(s). Passing the original
    # namespace here would default cmd_destroy() to "all" because stop's
    # argument is named `profile`, not `target`.
    cmd_destroy(argparse.Namespace(target=target))
    print("==> Stopped and cleaned up.")
    return 0


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

    selected_vm: dict[str, Any] | None = None

    if target:
        try:
            target_vmid = int(target)
            for v in running_vms:
                if v["vmid"] == target_vmid:
                    selected_vm = v
                    break
        except ValueError:
            pass

        if not selected_vm:
            target_lower = target.lower()
            for v in running_vms:
                if v["type"] == target_lower or (v["run_id"] and v["run_id"].startswith(target_lower)):
                    selected_vm = v
                    break

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
        # Check if key is available on pve server run directory
        remote_key_check = f"sudo test -f /var/lib/ra8-lab/{selected_vm['type']}/id_ed25519"
        if subprocess.run(["ssh", "-o", "BatchMode=yes", SSH_ALIAS, remote_key_check]).returncode == 0:
            # Connect via SSH jumping through pve with remote key
            ip = selected_vm["ip"]
            user = "Administrator" if selected_vm["type"] == "windows" else DEFAULT_USER
            ssh_cmd = (
                f"ssh -i /var/lib/ra8-lab/{selected_vm['type']}/id_ed25519 "
                "-o BatchMode=yes -o StrictHostKeyChecking=no "
                "-o UserKnownHostsFile=/dev/null -o LogLevel=ERROR "
                f"{user}@{ip}"
            )
            if remaining_cmd:
                ssh_cmd += f" {' '.join(remaining_cmd)}"
            os.execvp("ssh", ["ssh", "-t", SSH_ALIAS, f"sudo {ssh_cmd}"])
            return 0

        sys.stderr.write(
            f"error: could not find an SSH key for run '{selected_vm['run_id']}'.\n"
        )
        return 1

    ip = selected_vm["ip"]
    vmid = selected_vm["vmid"]
    user = "Administrator" if selected_vm["type"] == "windows" else DEFAULT_USER
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
        f"{user}@{ip}",
    ]
    if remaining_cmd:
        ssh_args.extend(remaining_cmd)

    os.execvp("ssh", ssh_args)
    return 0


def cmd_destroy(args: argparse.Namespace) -> int:
    target = getattr(args, "target", "all")
    all_vms = get_active_vms()
    target_vms: list[dict[str, Any]] = []

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
            shutil.rmtree(d, ignore_errors=True)
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

    parser = argparse.ArgumentParser(description="Proxmox Lab CI & VM Management")
    subparsers = parser.add_subparsers(dest="subcommand", required=True)

    # list
    subparsers.add_parser("list", help="List active or preserved lab VMs")

    # ci
    ci_parser = subparsers.add_parser("ci", help="Run CI on Proxmox and stream live logs (Ctrl+C detaches)")
    ci_parser.add_argument("profile", nargs="?", default="linux", choices=["linux", "windows"], help="CI profile")
    ci_parser.add_argument("--keep", action="store_true", help="Keep VM after run finishes")

    # start
    start_parser = subparsers.add_parser("start", help="Start CI in background on Proxmox and return immediately")
    start_parser.add_argument("profile", nargs="?", default="linux", choices=["linux", "windows"], help="CI profile")
    start_parser.add_argument("--keep", action="store_true", help="Keep VM after run finishes")

    # logs
    logs_parser = subparsers.add_parser("logs", help="Stream live CI logs from Proxmox")
    logs_parser.add_argument("profile", nargs="?", default="linux", choices=["linux", "windows"], help="CI profile")

    # status
    subparsers.add_parser("status", help="Show status of running background CI jobs on Proxmox")

    # stop
    stop_parser = subparsers.add_parser("stop", help="Stop and cancel active CI run(s) on Proxmox")
    stop_parser.add_argument("profile", nargs="?", default="all", help="Profile to stop (linux, windows, or all)")

    # destroy
    destroy_parser = subparsers.add_parser("destroy", help="Tear down preserved lab VMs and network")
    destroy_parser.add_argument("target", nargs="?", default="all", help="VM ID, type, or 'all'")

    args = parser.parse_args()
    if args.subcommand == "list":
        sys.exit(cmd_list(args))
    elif args.subcommand == "ci":
        sys.exit(cmd_ci(args))
    elif args.subcommand == "start":
        sys.exit(cmd_start(args))
    elif args.subcommand == "logs":
        sys.exit(cmd_logs(args))
    elif args.subcommand == "status":
        sys.exit(cmd_status(args))
    elif args.subcommand == "stop":
        sys.exit(cmd_stop(args))
    elif args.subcommand == "ssh":
        sys.exit(cmd_ssh(args))
    elif args.subcommand == "destroy":
        sys.exit(cmd_destroy(args))


if __name__ == "__main__":
    main()
