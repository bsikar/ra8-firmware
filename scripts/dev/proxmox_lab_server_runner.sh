#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Server-side Proxmox Lab CI runner.
# Runs directly on the Proxmox VE host (pve) in the background.
# Manages the isolated VM lifecycle, network boundaries, and streams live metrics.

set -euo pipefail
umask 077

PROFILE="${1:-linux}"
RUN_ID="${2:-$(od -An -N8 -tx1 /dev/urandom 2>/dev/null | tr -d ' \n')}"
ARCHIVE_PATH="${3:-/var/lib/ra8-lab/$PROFILE/source.tar}"
KEEP="${4:-false}"

export PATH="/usr/sbin:/usr/bin:/sbin:/bin:$PATH"

LOG_DIR="/var/log/ra8-lab"
RUN_DIR="/var/lib/ra8-lab/$PROFILE"
mkdir -p "$LOG_DIR" "$RUN_DIR"

LOG_FILE="$LOG_DIR/$PROFILE.log"
STATUS_FILE="$LOG_DIR/$PROFILE.status"
PID_FILE="$LOG_DIR/$PROFILE.pid"

# Truncate or initialize log for this run
echo "=== Starting disposable $PROFILE CI run $RUN_ID ===" > "$LOG_FILE"
exec >> "$LOG_FILE" 2>&1

echo "$$" > "$PID_FILE"
echo "RUNNING" > "$STATUS_FILE"

ts() { date +"[%Y-%m-%d %H:%M:%S]"; }

echo "$(ts) Initializing background execution for profile '$PROFILE' (PID: $$)..."

if [[ "$PROFILE" == "linux" ]]; then
  VM_ID=9000
  TEMPLATE_ID=9001
  BRIDGE="vmbr9"
  SUBNET="10.250.9.0/24"
  GATEWAY="10.250.9.1"
  GUEST_IP="10.250.9.10"
  GUEST_USER="terraform-lab"
elif [[ "$PROFILE" == "windows" ]]; then
  VM_ID=9010
  TEMPLATE_ID=9011
  BRIDGE="vmbr8"
  SUBNET="10.250.8.0/24"
  GATEWAY="10.250.8.1"
  GUEST_IP="10.250.8.20"
  GUEST_USER="Administrator"
  GUEST_PASSWORD="${RA8_LAB_WINDOWS_PASSWORD:-}"
  if [[ -z "$GUEST_PASSWORD" ]]; then
    echo "$(ts) error: RA8_LAB_WINDOWS_PASSWORD must be provided through the runner environment."
    exit 1
  fi
else
  echo "$(ts) error: unknown profile '$PROFILE'"
  exit 1
fi

NFT_TABLE="ra8_lab_ci_${RUN_ID}"

# The lifecycle driver normally prevents duplicate starts, but the server-side
# runner is also an entry point in its own right. Hold a profile-scoped lock so
# a retried launcher cannot create two runners for the same fixed VMID.
LOCK_FILE="/var/lock/ra8-lab-${PROFILE}.lock"
exec 9>"$LOCK_FILE"
if ! flock -n 9; then
  echo "$(ts) Another $PROFILE CI runner already owns $LOCK_FILE."
  exit 75
fi

cleanup() {
  local exit_code=$?
  echo "$(ts) Cleaning up $PROFILE CI run $RUN_ID (exit code $exit_code)..."
  if [[ "$KEEP" != "true" ]]; then
    if qm status "$VM_ID" >/dev/null 2>&1; then
      echo "$(ts) Stopping and destroying VM $VM_ID..."
      qm stop "$VM_ID" --timeout 15 >/dev/null 2>&1 || true
      for _ in {1..30}; do
        [[ "$(qm status "$VM_ID" 2>/dev/null | awk '{print $2}')" == "stopped" ]] && break
        sleep 1
      done
      qm set "$VM_ID" --protection 0 >/dev/null 2>&1 || true
      qm destroy "$VM_ID" --purge 1 >/dev/null 2>&1 || true
    fi
    if ip link show "$BRIDGE" >/dev/null 2>&1; then
      echo "$(ts) Tearing down $BRIDGE and firewall..."
      ip addr flush dev "$BRIDGE" scope global >/dev/null 2>&1 || true
      ip link delete "$BRIDGE" type bridge >/dev/null 2>&1 || true
    fi
    nft delete table ip "$NFT_TABLE" >/dev/null 2>&1 || true
  else
    echo "$(ts) Preserving VM $VM_ID and network $BRIDGE (--keep=true)"
  fi
  rm -f "$PID_FILE"
  if ((exit_code == 0)); then
    echo "SUCCESS" > "$STATUS_FILE"
    echo "$(ts) === CI COMPLETED: SUCCESS ==="
  else
    echo "FAILED" > "$STATUS_FILE"
    echo "$(ts) === CI COMPLETED: FAILED (exit code $exit_code) ==="
  fi
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# 1. Setup network isolation
echo "$(ts) Configuring network isolation ($BRIDGE, $SUBNET)..."
if ! ip link show "$BRIDGE" >/dev/null 2>&1; then
  ip link add "$BRIDGE" type bridge
fi
ip addr replace "$GATEWAY/24" dev "$BRIDGE"
ip link set "$BRIDGE" up
sysctl -w net.ipv4.ip_forward=1 >/dev/null

nft -f - <<EOF
table ip $NFT_TABLE {
  set lab_ingress {
    type ifname
    elements = { "$BRIDGE", "fwbr${VM_ID}i0", "fwln${VM_ID}i0", "fwpr${VM_ID}p0" }
  }

  chain input {
    type filter hook input priority -100; policy accept;
    ct state established,related accept
    iifname @lab_ingress tcp dport { 3142, 8080 } accept
    iifname @lab_ingress drop
  }

  chain forward {
    type filter hook forward priority -100; policy accept;
    ct state established,related accept
    iifname != @lab_ingress accept
    iifname @lab_ingress ip daddr { 10.0.0.0/8, 100.64.0.0/10, 127.0.0.0/8, 169.254.0.0/16, 172.16.0.0/12, 192.0.0.0/24, 192.0.2.0/24, 192.168.0.0/16, 198.18.0.0/15, 198.51.100.0/24, 203.0.113.0/24, 224.0.0.0/4, 240.0.0.0/4 } drop
    iifname @lab_ingress oifname "vmbr0" udp dport 53 accept
    iifname @lab_ingress oifname "vmbr0" tcp dport 53 accept
    iifname @lab_ingress oifname "vmbr0" tcp dport { 80, 443 } accept
    iifname @lab_ingress drop
  }

  chain postrouting {
    type nat hook postrouting priority srcnat; policy accept;
    oifname "vmbr0" ip saddr $SUBNET masquerade
  }
}
EOF

# 2. SSH key generation for this run
KEY_FILE="$RUN_DIR/id_ed25519"
rm -f "$KEY_FILE" "$KEY_FILE.pub"
ssh-keygen -q -t ed25519 -N '' -C "ra8-lab-$RUN_ID" -f "$KEY_FILE"

# 3. Clone and configure VM
echo "$(ts) Cloning VM $TEMPLATE_ID -> $VM_ID (ra8-lab-$PROFILE-$RUN_ID)..."
clone_full=1
if [[ "$PROFILE" == "windows" ]]; then
  # Windows' 64 GiB template is immutable and disposable runs do not need an
  # independent disk copy; linked clones avoid spending minutes copying it.
  clone_full=0
fi
clone_args=(qm clone "$TEMPLATE_ID" "$VM_ID" --name "ra8-lab-$PROFILE-$RUN_ID" --pool ra8-tf-lab --full "$clone_full")
if ((clone_full)); then
  clone_args+=(--storage ra8-tf-lab)
fi
"${clone_args[@]}"
if [[ "$PROFILE" == "windows" ]]; then
  # Server Core, the native toolchain, WSL2, and Podman builds exceed the
  # template's 8 GiB once Windows and the Linux VM are resident together.
  echo "$(ts) Allocating 16 GiB to the disposable Windows CI guest."
  qm set "$VM_ID" --memory 16384
  host_cpu_model=$(awk -F': ' '/^model name[[:space:]]*:/ { print $2; exit }' /proc/cpuinfo)
  case "$host_cpu_model" in
    *"12th Gen Intel"*|*"13th Gen Intel"*|*"14th Gen Intel"*)
      # Nested Hyper-V/WSL2 can hang in recovery on hybrid Intel CPUs when the
      # host's WAITPKG CPUID bit is exposed. Present a Skylake-compatible CPUID
      # to the disposable Windows guest while preserving VMX and Hyper-V flags.
      echo "$(ts) Applying the Windows nested-virtualization CPU workaround for $host_cpu_model."
      qm set "$VM_ID" --args "-cpu host,hv_passthrough,level=30,-waitpkg"
      ;;
  esac
  # Server Core plus the WSL2 CI toolchain and disposable container image need
  # more working space than the 64 GiB base template provides.
  qm resize "$VM_ID" sata0 +64G
fi
qm set "$VM_ID" --description "Disposable RA8 lab VM; RA8_LAB_RUN=$RUN_ID"
tags="terraform,ra8-lab,run-$RUN_ID"
if [[ "$PROFILE" == "windows" ]]; then
  tags+=",windows"
fi
qm set "$VM_ID" --tags "$tags"
# The runner's nftables table is the authoritative isolation boundary. Leaving
# Proxmox's per-interface firewall enabled here blocks first-boot ARP/SSH on
# the private bridge before cloud-init can finish configuring the guest.
net_model="virtio"
if [[ "$PROFILE" == "windows" ]]; then
  # Windows Server Core has no inbox VirtIO network driver. Use the emulated
  # Intel adapter so first boot can reach native WinRM without a GUI/tool MSI.
  net_model="e1000"
fi
qm set "$VM_ID" --net0 "$net_model,bridge=$BRIDGE,firewall=0,rate=10"

qm set "$VM_ID" --ide2 "ra8-tf-lab:cloudinit"
  qm set "$VM_ID" --ipconfig0 "ip=$GUEST_IP/24,gw=$GATEWAY"
  qm set "$VM_ID" --nameserver "1.1.1.1"
  qm set "$VM_ID" --ciuser "$GUEST_USER"
  qm set "$VM_ID" --sshkeys "$KEY_FILE.pub"

echo "$(ts) Starting VM $VM_ID..."
qm start "$VM_ID"

# 4. Wait for the guest management endpoint
SSH_OPTS=(-i "$KEY_FILE" -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=3)
ssh_ready=0
if [[ "$PROFILE" == "linux" ]]; then
  echo "$(ts) Waiting for guest SSH to $GUEST_USER@$GUEST_IP..."
  for _ in {1..90}; do
    if ssh "${SSH_OPTS[@]}" "$GUEST_USER@$GUEST_IP" true >/dev/null 2>&1; then
      ssh_ready=1
      break
    fi
    sleep 2
  done
else
  echo "$(ts) Waiting for guest WinRM at $GUEST_IP:5985..."
  for _ in {1..90}; do
    if timeout 3 bash -c "</dev/tcp/$GUEST_IP/5985" >/dev/null 2>&1; then
      ssh_ready=1
      break
    fi
    sleep 2
  done
fi

if ((!ssh_ready)); then
  echo "$(ts) Timed out waiting for the guest management endpoint."
  exit 1
fi
echo "$(ts) Guest management endpoint is online."

# 5. Stage code into guest. Linux runs the containerized CI directly; Windows
# is provisioned by the repository's Ansible playbook from the PVE controller.
if [[ "$PROFILE" == "linux" ]]; then
  echo "$(ts) Uploading repository archive to Linux guest..."
  scp "${SSH_OPTS[@]}" "$ARCHIVE_PATH" "$GUEST_USER@$GUEST_IP:/tmp/source.tar"
fi

if [[ "$PROFILE" == "linux" ]]; then
  echo "$(ts) Preparing Linux guest CI substrate and dependencies..."
  ssh "${SSH_OPTS[@]}" "$GUEST_USER@$GUEST_IP" bash -s -- "$GATEWAY" <<'GUEST_SETUP'
set -euo pipefail
gateway="${1:-10.250.9.1}"

# Auto-detect local lab apt cache proxy on host gateway
if curl -s --connect-timeout 1 "http://$gateway:3142" >/dev/null 2>&1; then
  echo "Acquire::http::Proxy \"http://$gateway:3142\";" | sudo tee /etc/apt/apt.conf.d/01proxy >/dev/null
fi

# Configure systemd-resolved to use approved 1.1.1.1 DNS egress
sudo mkdir -p /etc/systemd/resolved.conf.d
sudo bash -c 'cat > /etc/systemd/resolved.conf.d/lab-dns.conf <<EOF
[Resolve]
DNS=1.1.1.1
EOF'
sudo systemctl restart systemd-resolved 2>/dev/null || true

# Expand disk partition
root_dev="$(findmnt -n -o SOURCE / 2>/dev/null || echo /dev/sda1)"
disk_name="$(lsblk -no PKNAME "$root_dev" 2>/dev/null || echo sda)"
sudo growpart "/dev/$disk_name" 1 2>/dev/null || true
sudo resize2fs "$root_dev" 2>/dev/null || true

# Subordinate IDs for rootless Podman
if ! grep -q "^$USER:" /etc/subuid 2>/dev/null; then
  sudo usermod --add-subuids 100000-165535 --add-subgids 100000-165535 "$USER"
fi

# Ensure substrate packages
if ! command -v podman >/dev/null 2>&1 || ! command -v git-lfs >/dev/null 2>&1; then
  sudo apt-get update -qq
  sudo apt-get install -y -qq podman git git-lfs python3 curl ca-certificates fuse-overlayfs
fi

# Initialize git-lfs
sudo git lfs install --system >/dev/null 2>&1 || true

# Configure container runtime configs
mkdir -p ~/.config/containers
cat > ~/.config/containers/containers.conf <<'EOF'
[containers]
[engine]
cgroup_manager = "cgroupfs"
events_logger = "file"
EOF

cat > ~/.config/containers/storage.conf <<'EOF'
[storage]
driver = "overlay"
[storage.options.overlay]
mount_program = "/usr/bin/fuse-overlayfs"
EOF

# Unpack checkout
rm -rf ~/ra8-lab-ci
mkdir -p ~/ra8-lab-ci
tar -xf /tmp/source.tar -C ~/ra8-lab-ci
rm -f /tmp/source.tar
cd ~/ra8-lab-ci
git init -q --initial-branch=main
git config user.name "ra8-lab-ci"
git config user.email "ci@localhost"
git config maintenance.auto false
git config gc.auto 0
git add -A -f
git commit --allow-empty -q -m "Disposable CI snapshot"
GUEST_SETUP

  echo "$(ts) Launching CI suite inside container..."
  # Start CI in background inside guest and persist its exit code. A later SSH
  # session cannot wait(2) for a process it did not create, so waiting on the
  # PID from the monitor loop would report a false failure after successful CI.
  ssh "${SSH_OPTS[@]}" "$GUEST_USER@$GUEST_IP" bash -s <<'GUEST_LAUNCH'
set -u
rm -f "$HOME/ci.pid" "$HOME/ci.exit"
nohup /bin/bash -p -c '
  env RA8_CONTAINER_RUNTIME="sudo podman" /bin/bash -p "$HOME/ra8-lab-ci/scripts/ci/devcontainer_run.sh" -- just ci
  rc=$?
  echo "$rc" > "$HOME/ci.exit"
  exit "$rc"
' > "$HOME/ci.log" 2>&1 < /dev/null &
echo $! > "$HOME/ci.pid"
GUEST_LAUNCH

  echo "$(ts) Monitoring CI execution and system metrics..."
  ci_finished=0
  ci_exit=0
  guest_log_lines=0
  stream_guest_log() {
    local output line_count marker
    marker="__RA8_LOG_END_${guest_log_lines}__"
    output=$(ssh "${SSH_OPTS[@]}" "$GUEST_USER@$GUEST_IP" \
      "tail -n +$((guest_log_lines + 1)) ~/ci.log 2>/dev/null; printf '%s' '$marker'" || true)
    output=${output%"$marker"}
    if [[ -z "$output" ]]; then
      return 0
    fi
    while IFS= read -r line || [[ -n "$line" ]]; do
      echo "$(ts) [ra8-lab-ci] $line"
    done < <(printf '%s' "$output")
    line_count=$(printf '%s' "$output" | wc -l)
    if [[ "$output" != *$'\n' ]]; then
      line_count=$((line_count + 1))
    fi
    guest_log_lines=$((guest_log_lines + line_count))
  }

  while true; do
    sleep 10
    # Check if process is still running
    if ! ssh "${SSH_OPTS[@]}" "$GUEST_USER@$GUEST_IP" "kill -0 \$(cat ~/ci.pid 2>/dev/null) 2>/dev/null"; then
      ci_finished=1
      # Retrieve the exit status written by the guest-side wrapper.
      ci_exit=$(ssh "${SSH_OPTS[@]}" "$GUEST_USER@$GUEST_IP" "cat ~/ci.exit" 2>/dev/null || echo 1)
      break
    fi

    # Query live metrics from guest
    metric=$(ssh "${SSH_OPTS[@]}" "$GUEST_USER@$GUEST_IP" python3 <<'PYEOF' 2>/dev/null || true
import os, time, shutil, subprocess
l = ', '.join(f'{x:.2f}' for x in os.getloadavg())
t, u, f = shutil.disk_usage('/')
m = {k.rstrip(':'): int(v) for k, v in (x.split()[:2] for x in open('/proc/meminfo'))}
tot = m.get('MemTotal', 0) / 1048576
av = m.get('MemAvailable', 0) / 1048576
used = tot - av
rp = (used / tot * 100) if tot else 0
c1 = [int(x) for x in open('/proc/stat').readline().split()[1:5]]
time.sleep(0.05)
c2 = [int(x) for x in open('/proc/stat').readline().split()[1:5]]
db = (c2[0] + c2[1] + c2[2]) - (c1[0] + c1[1] + c1[2])
dt = sum(c2) - sum(c1)
cpu = f'{(db / dt * 100):.0f}%' if dt > 0 else '0%'
top_cmd = subprocess.run("ps -eo comm --sort=-pcpu | awk 'NR>1 && !/^(ps|python3|awk|sshd|systemd|kworker|bash|sh|init|tmux)/ {print; exit}'", shell=True, capture_output=True, text=True).stdout.strip()
task = f' | active: {top_cmd}' if top_cmd else ''
print(f'load: [{l}] | cpu: {cpu} | ram: {used:.1f}G/{tot:.1f}G ({rp:.0f}%) | disk: {f/1073741824:.1f}G free ({f/t*100:.0f}%){task}')
PYEOF
)
    if [[ -n "$metric" ]]; then
      echo "$(ts) [ra8-lab-linux] $metric"
    fi
    stream_guest_log
  done

  # Drain lines written between the last poll and the guest exit marker.
  stream_guest_log

  if ((ci_exit != 0)); then
    echo "$(ts) Guest CI command failed with exit code $ci_exit."
    exit "$ci_exit"
  fi

elif [[ "$PROFILE" == "windows" ]]; then
  echo "$(ts) Preparing the PVE-side Ansible controller for Windows CI..."
  CONTROLLER_DIR="$RUN_DIR/ansible-controller"
  ANSIBLE_VENV="$RUN_DIR/ansible-venv"
  COLLECTIONS_DIR="$CONTROLLER_DIR/collections"
  mkdir -p "$CONTROLLER_DIR"
  tar -xf "$ARCHIVE_PATH" -C "$CONTROLLER_DIR"

  if [[ ! -x "$ANSIBLE_VENV/bin/ansible-playbook" ]]; then
    apt-get update -qq
    apt-get install -y -qq python3-venv python3-pip
    python3 -m venv "$ANSIBLE_VENV"
    "$ANSIBLE_VENV/bin/pip" install --disable-pip-version-check --no-cache-dir \
      'ansible-core>=2.18,<2.19' pywinrm
  fi
  ANSIBLE_CONFIG="$CONTROLLER_DIR/infra/ansible/ansible.cfg" \
    "$ANSIBLE_VENV/bin/ansible-galaxy" collection install \
    -r "$CONTROLLER_DIR/infra/ansible/requirements.yml" \
    -p "$COLLECTIONS_DIR"

  INVENTORY="$RUN_DIR/windows-inventory.ini"
  cat > "$INVENTORY" <<EOF
[lab_windows]
ra8-lab-windows ansible_host=$GUEST_IP ansible_port=5985 ansible_user=$GUEST_USER ansible_password=$GUEST_PASSWORD ansible_connection=winrm ansible_winrm_transport=ntlm ansible_winrm_server_cert_validation=ignore

[lab_windows:vars]
ansible_host_key_checking=False
EOF

  echo "$(ts) Provisioning Windows Server Core and running CI through Ansible..."
  ANSIBLE_CONFIG="$CONTROLLER_DIR/infra/ansible/ansible.cfg" \
    ANSIBLE_COLLECTIONS_PATH="$COLLECTIONS_DIR" \
    "$ANSIBLE_VENV/bin/ansible-playbook" \
      -i "$INVENTORY" \
      "$CONTROLLER_DIR/infra/ansible/playbooks/proxmox-lab-windows.yml" \
      -e "lab_ci_source_archive=$ARCHIVE_PATH" \
      -e "lab_ci_user=$GUEST_USER"
fi

echo "$(ts) CI execution completed successfully."
exit 0
