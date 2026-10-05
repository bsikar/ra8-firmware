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
import shlex
import shutil
import subprocess
import sys
import tempfile
from typing import Any

SSH_ALIAS = "pve"

# The Windows lab credential lives in a root-owned file on the Proxmox host and
# is read at the point of use by the runner. This controller never reads it,
# never forwards it, and reports presence as a boolean and nothing else.
WINDOWS_CREDENTIAL_FILE = "/etc/ra8-lab/windows-password"
WINDOWS_CREDENTIAL_ENV = "RA8_LAB_WINDOWS_PASSWORD"
DEFAULT_USER = "terraform-lab"
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))

WINDOWS_VM_ID = 9021
PROTECTED_TEMPLATE_IDS = {9001, 9011}
LINUX_RESERVED_IDS = {9001, 9011, WINDOWS_VM_ID}


def linux_vm_identity(value: str | int) -> tuple[int, str]:
    """Validate a disposable Linux VMID and return its VMID/address pair."""
    raw = str(value)
    if not re.fullmatch(r"90[0-9]{2}", raw):
        raise ValueError("Linux VMID must be a decimal ID from 9000 through 9099")
    vmid = int(raw)
    if vmid in LINUX_RESERVED_IDS:
        raise ValueError(f"VMID {vmid} is reserved for a template or Windows guest")
    return vmid, f"10.250.9.{vmid - 9000 + 10}"


def run_paths(profile: str, run_id: str, vmid: int) -> dict[str, str]:
    """Build validated per-run controller paths and a per-VM ownership lock."""
    if profile not in {"linux", "windows"} or not re.fullmatch(r"[0-9a-f]{16}", run_id):
        raise ValueError("invalid lab profile or run ID")
    if not 9000 <= vmid <= 9099:
        raise ValueError("VMID is outside the reserved lab range")
    run_dir = f"/var/lib/ra8-lab/{profile}/{run_id}"
    log_dir = f"/var/log/ra8-lab/{profile}"
    return {
        "run_dir": run_dir,
        "archive": f"{run_dir}/source.tar",
        "runner": f"{run_dir}/runner.sh",
        "log": f"{log_dir}/{run_id}.log",
        "status": f"{log_dir}/{run_id}.status",
        "pid": f"{log_dir}/{run_id}.pid",
        "lock": f"/var/lock/ra8-lab-{profile}-{vmid}.lock",
    }


def vm_ip(vmid: int) -> str:
    """Resolve an approved lab VMID to its guest address."""
    if 9000 <= vmid <= 9099 and vmid not in PROTECTED_TEMPLATE_IDS:
        return f"10.250.9.{vmid - 9000 + 10}"
    raise ValueError(f"VMID {vmid} is outside the guest address allowlist")


def linux_network_cleanup_action(other_run_tables: bool, bridge_ports: bool) -> str:
    """Choose whether a destroyed Linux run may remove the shared bridge."""
    return "preserve" if other_run_tables or bridge_ports else "remove"


def get_active_vms(*, strict: bool = False) -> list[dict[str, Any]]:
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
        if strict:
            raise
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
            elif "windows" in name or vmid == WINDOWS_VM_ID:
                vm_type = "windows"
            else:
                vm_type = "linux"
            vms.append({
                "vmid": vmid,
                "name": name,
                "type": vm_type,
                "status": status,
                "run_id": run_id,
                "ip": "-" if vm_type == "template" else vm_ip(vmid),
            })

    return vms


def linux_run_cleanup_script(vmid: int, run_id: str) -> str:
    """Remove only a destroyed Linux run's firewall table and last-run bridge."""
    if not 9000 <= vmid <= 9099 or vmid in LINUX_RESERVED_IDS:
        raise ValueError("VMID is outside the disposable Linux range")
    if not re.fullmatch(r"[0-9a-f]{16}", run_id):
        raise ValueError("invalid lab run ID")
    script = r"""set -euo pipefail
exec 9>/run/lock/ra8-lab-ci-vmbr9.lock
flock -x 9
vmid=@VMID@
table=@TABLE@
state_dir=/run/ra8-lab-ci
state=$state_dir/vmbr9.state
bridge=vmbr9
if qm config "$vmid" >/dev/null 2>&1; then
    echo 'refusing to remove a run firewall table while its VM still exists' >&2
    exit 1
fi
[[ "$table" =~ ^ra8_lab_ci_[0-9a-f]{16}$ ]] || { echo 'invalid run firewall table' >&2; exit 1; }
[[ -d "$state_dir" && ! -L "$state_dir" && "$(stat -c %u "$state_dir")" == 0 && "$(stat -c %a "$state_dir")" == 700 ]] || { echo 'refusing Linux lab cleanup without its trusted state directory' >&2; exit 1; }
[[ -f "$state" && ! -L "$state" && "$(stat -c %u "$state")" == 0 && "$(stat -c %a "$state")" == 600 ]] || { echo 'refusing Linux lab cleanup without its trusted state marker' >&2; exit 1; }
read -r old_forward saved_gateway saved_subnet <"$state"
[[ ("$old_forward" == 0 || "$old_forward" == 1) && "$saved_gateway" == 10.250.9.1 && "$saved_subnet" == 10.250.9.0/24 ]] || { echo 'refusing Linux lab cleanup with a mismatched state marker' >&2; exit 1; }
if nft list table ip "$table" >/dev/null 2>&1; then
    nft delete table ip "$table"
fi
other_tables=$(nft list tables | awk '$1 == "table" && $2 == "ip" && $3 ~ /^ra8_lab_ci_[0-9a-f]{16}$/ {print $3}')
children=$(ip -o link show master "$bridge" 2>/dev/null | awk -F': ' '{print $2}')
if [[ -n "$other_tables" || -n "$children" ]]; then
    echo 'Preserving shared vmbr9; other run tables or guest ports remain.'
    exit 0
fi
[[ "$(ip -o -4 addr show dev "$bridge" | awk 'NR == 1 {print $4}')" == 10.250.9.1/24 &&
   "$(ip -d link show dev "$bridge" | grep -c bridge)" -gt 0 ]] || { echo 'refusing to remove an unexpected vmbr9' >&2; exit 1; }
ip addr flush dev "$bridge" scope global
ip link set "$bridge" down
ip link delete "$bridge" type bridge
sysctl -w "net.ipv4.ip_forward=$old_forward" >/dev/null
rm -f -- "$state"
"""
    return script.replace("@VMID@", str(vmid)).replace("@TABLE@", f"ra8_lab_ci_{run_id}")


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


def windows_credential_present() -> bool:
    """Report whether the Proxmox host holds the Windows lab credential.

    Runs a test on the host that answers with an exit status only. Nothing in
    the command reads, prints, or transports the value, so a false answer and a
    true answer differ by one bit and by nothing else.
    """
    probe = (
        f"sudo -n test -f {WINDOWS_CREDENTIAL_FILE} && "
        f"sudo -n test -s {WINDOWS_CREDENTIAL_FILE} && "
        f"sudo -n test -O {WINDOWS_CREDENTIAL_FILE} && "
        f"[ \"$(sudo -n stat -c %a {WINDOWS_CREDENTIAL_FILE})\" = 600 ]"
    )
    completed = subprocess.run(
        ["ssh", "-o", "BatchMode=yes", "-o", "RequestTTY=no", SSH_ALIAS, probe],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return completed.returncode == 0


def check_windows_credential() -> int:
    """Gate a Windows run on the credential being in place, without reading it."""
    if os.environ.get(WINDOWS_CREDENTIAL_ENV):
        sys.stderr.write(
            f"error: {WINDOWS_CREDENTIAL_ENV} is set in this shell's environment.\n"
            "  The lab credential is no longer carried in the environment: a value there is\n"
            "  readable from the process table and survives in shell history and crash dumps.\n"
            f"  Unset it, and place the value on {SSH_ALIAS} at {WINDOWS_CREDENTIAL_FILE}.\n"
        )
        return 1
    if not windows_credential_present():
        sys.stderr.write(
            f"error: Windows lab credential not present on {SSH_ALIAS}.\n"
            f"  Expected a non-empty root-owned file at {WINDOWS_CREDENTIAL_FILE}, mode 0600.\n"
            "  Create it on the host as root, for example:\n"
            f"    install -d -m 700 -o root -g root {os.path.dirname(WINDOWS_CREDENTIAL_FILE)}\n"
            f"    install -m 600 -o root -g root /dev/null {WINDOWS_CREDENTIAL_FILE}\n"
            f"    # then write the value into {WINDOWS_CREDENTIAL_FILE} from a root editor\n"
        )
        return 1
    print(f"    Windows lab credential: present on {SSH_ALIAS} (value not read)")
    return 0


def cmd_start(args: argparse.Namespace) -> int:
    """Start CI execution asynchronously on the Proxmox server."""
    profile = args.profile.lower()
    if profile not in ("linux", "windows"):
        sys.stderr.write(f"error: profile must be 'linux' or 'windows', got '{profile}'\n")
        return 1

    try:
        vmid, guest_ip = linux_vm_identity(os.environ.get("RA8_LAB_LINUX_VM_ID", "9000")) if profile == "linux" else (WINDOWS_VM_ID, vm_ip(WINDOWS_VM_ID))
    except ValueError as err:
        sys.stderr.write(f"error: {err}\n")
        return 1

    if profile == "windows":
        print(f"==> Checking the Windows lab credential on {SSH_ALIAS}...")
        credential_status = check_windows_credential()
        if credential_status != 0:
            return credential_status

    run_id = os.urandom(8).hex()
    tar_path = os.path.join(tempfile.gettempdir(), f"ra8-lab-{profile}-{run_id}.tar")
    args.run_id = run_id
    args.vmid = vmid

    print(f"==> Packaging local workspace for {profile} CI (including uncommitted changes)...")
    size_bytes = build_source_archive(tar_path)
    print(f"    Payload packaged: {size_bytes / (1024 * 1024):.1f} MB")

    print(f"==> Uploading payload and runner to Proxmox host ({SSH_ALIAS})...")
    paths = run_paths(profile, run_id, vmid)
    remote_dir = paths["run_dir"]
    remote_log_dir = f"/var/log/ra8-lab/{profile}"
    runner_script = os.path.join(SCRIPT_DIR, "proxmox_lab_server_runner.sh")
    runner_remote = paths["runner"]

    setup_cmd = (
        f"sudo install -d -m 0750 -o \"$(whoami)\" -g \"$(id -gn)\" {shlex.quote(remote_dir)} "
        f"&& sudo install -d -m 0750 {shlex.quote(remote_log_dir)}"
    )
    subprocess.run(["ssh", "-o", "BatchMode=yes", SSH_ALIAS, setup_cmd], check=True)

    # PVE's restricted SSH service intermittently stalls the SFTP-backed
    # scp implementation during large uploads. Use the legacy SCP protocol
    # for these two controller-to-host transfers instead.
    subprocess.run(["scp", "-O", "-q", tar_path, f"{SSH_ALIAS}:{paths['archive']}"], check=True)
    subprocess.run(["scp", "-O", "-q", runner_script, f"{SSH_ALIAS}:{runner_remote}"], check=True)
    try:
        os.remove(tar_path)
    except OSError:
        pass

    keep_str = "true" if getattr(args, "keep", False) else "false"
    launch_cmd = (
        f"sudo -n chmod +x {shlex.quote(runner_remote)} && "
        f"(sudo -n nohup /bin/bash {shlex.quote(runner_remote)} {profile} {run_id} "
        f"{shlex.quote(paths['archive'])} {keep_str} {vmid} {shlex.quote(guest_ip)} "
        f"</dev/null >/dev/null 2>&1 &)"
    )
    subprocess.run(["ssh", "-o", "BatchMode=yes", SSH_ALIAS, launch_cmd], check=True)

    print(f"==> {profile.capitalize()} CI run started on {SSH_ALIAS} (run_id: {run_id})")
    print(f"    • Guest VMID/address:  {vmid} / {guest_ip}")
    print(f"    • Stream this run:     just infra::lab::logs {profile} {run_id}")
    print("    • Check status:        just infra::lab::status")
    print(f"    • Stop/cancel:         just infra::lab::stop {profile}")
    return 0


def cmd_logs(args: argparse.Namespace) -> int:
    """Stream live CI logs from the Proxmox server with safe non-killing Ctrl+C detach."""
    profile = getattr(args, "profile", "linux").lower()
    if profile not in {"linux", "windows"}:
        sys.stderr.write("error: profile must be 'linux' or 'windows'\n")
        return 1
    run_id = getattr(args, "run_id", None)
    if not run_id:
        run_id = None
        listing = subprocess.run(
            [
                "ssh", "-o", "BatchMode=yes", SSH_ALIAS,
                f"sudo find /var/log/ra8-lab/{profile} -maxdepth 1 -type f -name '*.log' -printf '%T@ %f\\n' | sort -rn | head -n 1 | awk '{{print $2}}'",
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        log_name = listing.stdout.strip()
        run_id = log_name[:-4] if log_name.endswith(".log") else ""
    if not re.fullmatch(r"[0-9a-f]{16}", run_id or ""):
        sys.stderr.write("error: no matching run log found, or run ID is invalid\n")
        return 1
    log_file = run_paths(profile, run_id, 9000)["log"]
    print(f"==> Attaching to {profile} CI log stream on {SSH_ALIAS}...")
    print("    (Press Ctrl+C at any time to detach without stopping the CI run)\n")

    tail_cmd = [
        "ssh",
        "-t",
        "-o", "LogLevel=ERROR",
        SSH_ALIAS,
        f"sudo test -f {shlex.quote(log_file)} || {{ echo 'No log file found for run {run_id}.'; exit 1; }}; sudo tail -n 50 -f {shlex.quote(log_file)}"
    ]
    try:
        proc = subprocess.run(tail_cmd)
        return proc.returncode
    except KeyboardInterrupt:
        print("\n^C\n==> Detached from log stream.")
        print("    The CI run is still executing on the server!")
        print(f"    • Re-attach logs:  just infra::lab::logs {profile} {run_id}")
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
      found=0
      for status_file in "/var/log/ra8-lab/$profile/"*.status; do
        [[ -f "$status_file" ]] || continue
        found=1
        run_id="${status_file##*/}"
        run_id="${run_id%.status}"
        pid_file="/var/log/ra8-lab/$profile/$run_id.pid"
        log_file="/var/log/ra8-lab/$profile/$run_id.log"
        status="$(cat "$status_file" 2>/dev/null || echo UNKNOWN)"
        if [[ -f "$pid_file" ]]; then
          pid="$(cat "$pid_file" 2>/dev/null || true)"
          if [[ "$pid" =~ ^[0-9]+$ ]] && kill -0 "$pid" 2>/dev/null; then
            status="RUNNING (PID: $pid)"
          elif [[ "$status" == RUNNING ]]; then
            status="STALE (runner PID is no longer active)"
          fi
        elif [[ "$status" == RUNNING ]]; then
          status="STALE (runner PID file is absent)"
        fi
        echo "${profile^^} $run_id: $status"
        if [[ -f "$log_file" ]]; then
          echo "  Last output: $(tail -n 1 "$log_file")"
        fi
      done
      [[ "$found" == 1 ]] || echo "${profile^^}: IDLE (no run records)"
    done
    """
    subprocess.run(
        ["ssh", "-o", "BatchMode=yes", "-o", "RequestTTY=no", SSH_ALIAS, "sudo -n /bin/bash -s"],
        input=status_script,
        text=True,
        check=False,
    )
    print()
    cmd_list(args)
    return 0


def cmd_stop(args: argparse.Namespace) -> int:
    """Stop running CI job(s) on Proxmox and tear down the environment."""
    target = getattr(args, "profile", "all").lower()
    run_id = getattr(args, "run_id", None)
    if target not in {"all", "", "linux", "windows"}:
        sys.stderr.write("error: profile must be 'linux', 'windows', or 'all'\n")
        return 1
    profiles = ["linux", "windows"] if target in ("all", "") else [target]
    if run_id and not re.fullmatch(r"[0-9a-f]{16}", run_id):
        sys.stderr.write("error: run ID must be exactly 16 lowercase hexadecimal characters\n")
        return 1

    for profile in profiles:
        print(f"==> Stopping {profile} CI background runner on {SSH_ALIAS}...")
        stop_script = f"""
        for pid_file in "/var/log/ra8-lab/{profile}/"*.pid; do
          [[ -f "$pid_file" ]] || continue
          if [[ -n "{run_id or ''}" && "${{pid_file##*/}}" != "{run_id or ''}.pid" ]]; then continue; fi
          run_id="${{pid_file##*/}}"
          run_id="${{run_id%.pid}}"
          status_file="/var/log/ra8-lab/{profile}/$run_id.status"
          pid=$(cat "$pid_file" 2>/dev/null || true)
          if [[ "$pid" =~ ^[0-9]+$ ]] && kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
            for _ in {{1..15}}; do
              kill -0 "$pid" 2>/dev/null || break
              sleep 1
            done
            kill -9 "$pid" 2>/dev/null || true
          fi
          rm -f "$pid_file"
          if [[ -f "$status_file" ]] && [[ "$(cat "$status_file" 2>/dev/null)" == "RUNNING" ]]; then
            printf 'FAILED\\n' > "$status_file"
          fi
        done
        """
        subprocess.run(
            ["ssh", "-o", "BatchMode=yes", "-o", "RequestTTY=no", SSH_ALIAS, "sudo -n /bin/bash -s"],
            input=stop_script,
            text=True,
            check=False,
        )

    # Destroy only the requested profile's VM(s). Passing the original
    # namespace here would default cmd_destroy() to "all" because stop's
    # argument is named `profile`, not `target`.
    destroy_target = run_id if run_id else target
    cmd_destroy(argparse.Namespace(target=destroy_target))
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
    try:
        all_vms = get_active_vms(strict=True)
    except subprocess.CalledProcessError:
        return 1
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
        run_id = selected_vm["run_id"]
        if not re.fullmatch(r"[0-9a-f]{16}", run_id):
            sys.stderr.write(f"error: no valid run ID is available for VM {selected_vm['vmid']}\n")
            return 1
        remote_key = f"/var/lib/ra8-lab/{selected_vm['type']}/{run_id}/id_ed25519"
        remote_key_check = f"sudo test -f {shlex.quote(remote_key)}"
        if subprocess.run(["ssh", "-o", "BatchMode=yes", SSH_ALIAS, remote_key_check]).returncode == 0:
            # Connect via SSH jumping through pve with remote key
            ip = selected_vm["ip"]
            user = "Administrator" if selected_vm["type"] == "windows" else DEFAULT_USER
            ssh_cmd = (
                f"ssh -i {shlex.quote(remote_key)} "
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
    try:
        all_vms = get_active_vms(strict=True)
    except subprocess.CalledProcessError:
        return 1
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
        if target not in ("", "all"):
            print("No guest matched the requested target; preserving lab network state.")
            return 0
    else:
        for vm in target_vms:
            vmid = vm["vmid"]
            print(f"Tearing down VM {vmid} ({vm['name']})...")
            run_id = vm["run_id"]
            if vm["type"] == "linux" and not re.fullmatch(r"[0-9a-f]{16}", run_id):
                sys.stderr.write(f"error: refusing to destroy Linux VM {vmid} without a valid run ID\n")
                return 1
            remote_cmd = f"""
            set -euo pipefail
            if qm status {vmid} >/dev/null 2>&1; then
                config=$(qm config {vmid})
                name=$(awk -F': ' '$1 == "name" {{print substr($0, index($0, ": ") + 2); exit}}' <<<"$config")
                [[ "$name" == {shlex.quote(vm['name'])} ]] || {{ echo 'refusing to destroy a VM whose name changed' >&2; exit 1; }}
                description=$(awk -F': ' '$1 == "description" {{print substr($0, index($0, ": ") + 2); exit}}' <<<"$config")
                [[ "$description" == *{shlex.quote('RA8_LAB_RUN=' + run_id)}* ]] || {{ echo 'refusing to destroy a VM without its lab run marker' >&2; exit 1; }}
                qm stop {vmid} >/dev/null 2>&1 || true
                for _ in {{1..30}}; do
                    [[ "$(qm status {vmid} | awk '{{print $2}}')" == "stopped" ]] && break
                    sleep 1
                done
                [[ "$(qm status {vmid} | awk '{{print $2}}')" == "stopped" ]] || {{ echo 'VM did not stop; refusing to destroy it' >&2; exit 1; }}
                qm set {vmid} --protection 0 >/dev/null
                qm destroy {vmid} --purge 1 >/dev/null
                if qm config {vmid} >/dev/null 2>&1; then
                    echo 'VM still exists after destroy' >&2
                    exit 1
                fi
            fi
            """
            destroy_result = subprocess.run(
                ["ssh", "-o", "BatchMode=yes", "-o", "RequestTTY=no", SSH_ALIAS, "sudo -n /bin/bash -s"],
                input=remote_cmd,
                text=True,
                check=False,
            )
            if destroy_result.returncode != 0:
                sys.stderr.write(f"error: failed to destroy lab VM {vmid}\n")
                return destroy_result.returncode
            if vm["type"] == "linux":
                network_cmd = linux_run_cleanup_script(vmid, run_id)
                network_result = subprocess.run(
                    ["ssh", "-o", "BatchMode=yes", "-o", "RequestTTY=no", SSH_ALIAS, "sudo -n /bin/bash -s"],
                    input=network_cmd,
                    text=True,
                    check=False,
                )
                if network_result.returncode != 0:
                    sys.stderr.write(f"error: failed to clean up network for lab VM {vmid}\n")
                    return network_result.returncode

    # Clean up network if all lab VMs destroyed
    try:
        remaining = [v for v in get_active_vms(strict=True) if v["type"] != "template"]
    except subprocess.CalledProcessError:
        return 1
    if not remaining:
        print("Cleaning up lab bridges (vmbr8, vmbr9), nftables, and host routes...")
        net_cleanup = """
        set -euo pipefail
        state_dir=/run/ra8-lab-ci
        state=$state_dir/vmbr9.state
        restore_forward=""
        if ip link show vmbr9 >/dev/null 2>&1; then
            [[ -d "$state_dir" && ! -L "$state_dir" && "$(stat -c %u "$state_dir")" == 0 && "$(stat -c %a "$state_dir")" == 700 ]] || {
                echo 'refusing vmbr9 cleanup without its trusted lifecycle directory' >&2; exit 1;
            }
            [[ -f "$state" && ! -L "$state" && "$(stat -c %u "$state")" == 0 && "$(stat -c %a "$state")" == 600 ]] || {
                echo 'refusing vmbr9 cleanup without its trusted lifecycle marker' >&2; exit 1;
            }
            read -r restore_forward saved_gateway saved_subnet <"$state"
            [[ ("$restore_forward" == 0 || "$restore_forward" == 1) && "$saved_gateway" == 10.250.9.1 && "$saved_subnet" == 10.250.9.0/24 ]] || {
                echo 'refusing vmbr9 cleanup with a mismatched lifecycle marker' >&2; exit 1;
            }
            [[ "$(ip -o -4 addr show dev vmbr9 | awk '{print $4}')" == 10.250.9.1/24 &&
               "$(ip -d link show dev vmbr9 | grep -c bridge)" -gt 0 ]] || {
                echo 'refusing vmbr9 cleanup because its address or link type changed' >&2; exit 1;
            }
        elif [[ -e "$state" || -L "$state" ]]; then
            echo 'refusing cleanup with a stale vmbr9 lifecycle marker' >&2; exit 1;
        fi
        for br in vmbr8 vmbr9; do
            if ip link show "$br" >/dev/null 2>&1; then
                children="$(ip -o link show master "$br" 2>/dev/null | awk -F': ' '{print $2}')"
                [[ -z "$children" ]] || { echo "refusing to remove $br with attached ports: $children" >&2; exit 1; }
                ip addr flush dev "$br" scope global >/dev/null 2>&1 || true
                ip link delete "$br" type bridge >/dev/null 2>&1 || true
            fi
        done
        for table in $(nft list tables 2>/dev/null | awk '$1 == "table" && $3 ~ /^ra8_lab_ci_/ {print $3}'); do
            nft delete table ip "$table" >/dev/null 2>&1 || true
        done
        if [[ -n "$restore_forward" ]]; then
            sysctl -w "net.ipv4.ip_forward=$restore_forward" >/dev/null
            rm -f -- "$state"
        fi
        """
        cleanup_result = subprocess.run(
            ["ssh", "-o", "BatchMode=yes", "-o", "RequestTTY=no", SSH_ALIAS, "sudo -n /bin/bash -s"],
            input=net_cleanup,
            text=True,
            check=False,
        )
        if cleanup_result.returncode != 0:
            sys.stderr.write(f"error: Proxmox lab network cleanup failed (exit {cleanup_result.returncode})\n")
            return cleanup_result.returncode

    # Clean matching local run dirs
    for d in glob.glob(os.path.join(tempfile.gettempdir(), "ra8-lab-ci.*")):
        try:
            shutil.rmtree(d, ignore_errors=True)
        except OSError:
            pass

    print("Cleanup complete.")
    return 0


def main() -> None:
    if len(sys.argv) > 1 and sys.argv[1] == "--selftest":
        assert vm_ip(9000) == "10.250.9.10"
        assert vm_ip(9002) == "10.250.9.12"
        assert vm_ip(9099) == "10.250.9.109"
        assert vm_ip(9021) == "10.250.9.31"
        assert vm_ip(9010) == "10.250.9.20"
        assert linux_vm_identity("9002") == (9002, "10.250.9.12")
        paths_a = run_paths("linux", "aaaaaaaaaaaaaaaa", 9000)
        paths_b = run_paths("linux", "bbbbbbbbbbbbbbbb", 9002)
        assert all(paths_a[key] != paths_b[key] for key in ("run_dir", "archive", "runner", "log", "status", "pid"))
        assert paths_a["lock"] != paths_b["lock"]
        assert run_paths("linux", "aaaaaaaaaaaaaaaa", 9000)["lock"] == paths_a["lock"]
        assert not set(paths_a.values()) & set(paths_b.values())
        parser = argparse.ArgumentParser()
        subparsers = parser.add_subparsers(dest="subcommand", required=True)
        logs_parser = subparsers.add_parser("logs")
        logs_parser.add_argument("profile", nargs="?", default="linux", choices=["linux", "windows"])
        logs_parser.add_argument("run_id", nargs="?")
        assert parser.parse_args(["logs"]).run_id is None
        assert parser.parse_args(["logs", "linux", ""]).run_id == ""
        assert parser.parse_args(["logs", "linux", "aaaaaaaaaaaaaaaa"]).run_id == "aaaaaaaaaaaaaaaa"
        stop_parser = subparsers.add_parser("stop")
        stop_parser.add_argument("profile", nargs="?", default="all")
        stop_parser.add_argument("run_id", nargs="?")
        assert parser.parse_args(["stop", "linux", "aaaaaaaaaaaaaaaa"]).run_id == "aaaaaaaaaaaaaaaa"
        for protected in ("9001", "9011", "9021", "9100", "09002"):
            try:
                linux_vm_identity(protected)
            except ValueError:
                continue
            raise AssertionError(f"accepted protected/out-of-range Linux VMID {protected}")
        for protected_or_invalid in (9001, 9011, 9100):
            try:
                vm_ip(protected_or_invalid)
            except ValueError:
                continue
            raise AssertionError(f"accepted protected/out-of-range VMID {protected_or_invalid}")
        assert linux_network_cleanup_action(other_run_tables=True, bridge_ports=True) == "preserve"
        assert linux_network_cleanup_action(other_run_tables=False, bridge_ports=True) == "preserve"
        assert linux_network_cleanup_action(other_run_tables=True, bridge_ports=False) == "preserve"
        assert linux_network_cleanup_action(other_run_tables=False, bridge_ports=False) == "remove"
        cleanup_script = linux_run_cleanup_script(9002, "0123456789abcdef")
        assert 'nft delete table ip "$table"' in cleanup_script
        assert 'other_tables=$(nft list tables' in cleanup_script
        assert 'ip link delete "$bridge" type bridge' in cleanup_script
        for invalid_vmid in (9001, 9011, 9021, 9100):
            try:
                linux_run_cleanup_script(invalid_vmid, "0123456789abcdef")
            except ValueError:
                continue
            raise AssertionError(f"generated Linux cleanup for protected/out-of-range VMID {invalid_vmid}")
        print("proxmox_lab_manage.py --selftest: PASS")
        return
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
    logs_parser.add_argument("run_id", nargs="?", help="specific run ID")

    # status
    subparsers.add_parser("status", help="Show status of running background CI jobs on Proxmox")

    # stop
    stop_parser = subparsers.add_parser("stop", help="Stop and cancel active CI run(s) on Proxmox")
    stop_parser.add_argument("profile", nargs="?", default="all", help="Profile to stop (linux, windows, or all)")
    stop_parser.add_argument("run_id", nargs="?", help="specific run ID to stop")

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
