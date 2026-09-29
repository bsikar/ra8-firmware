#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Bake the `ra8-ci:latest` devcontainer image directly into the Debian golden template (VM 9001).
# Once baked, all disposable lab CI runs start in ~5 seconds with zero image-build overhead.

set -euo pipefail
umask 077

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

SSH_ALIAS="${SSH_ALIAS:-pve}"
ORIGINAL_TEMPLATE_ID=9001
BAKER_VM_ID=9005
STORAGE_ID="ra8-tf-lab"
POOL_ID="ra8-tf-lab"
BRIDGE="vmbr9"
SUBNET="10.250.9.0/24"
GATEWAY="10.250.9.1"
GUEST_IP="10.250.9.10"
GUEST_USER="terraform-lab"

info() { printf '\033[1;34m[INFO]\033[0m %s\n' "$*"; }
success() { printf '\033[1;32m[OK]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[WARN]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[ERROR]\033[0m %s\n' "$*" >&2; exit 1; }

remote_root() {
  ssh -o BatchMode=yes -o RequestTTY=no "$SSH_ALIAS" sudo -n /bin/bash -s -- "$@"
}

cleanup() {
  local rc=$?
  if ((rc != 0)); then
    warn "Cleaning up temporary baker VM $BAKER_VM_ID on failure..."
    remote_root "$BAKER_VM_ID" <<'REMOTE' || true
qm stop "$1" --timeout 10 >/dev/null 2>&1 || true
qm set "$1" --protection 0 >/dev/null 2>&1 || true
qm destroy "$1" --purge 1 >/dev/null 2>&1 || true
REMOTE
  fi
  exit "$rc"
}
trap cleanup EXIT INT TERM

info "Checking SSH connection to Proxmox host '$SSH_ALIAS'..."
if ! ssh -o BatchMode=yes -o RequestTTY=no "$SSH_ALIAS" /bin/true >/dev/null 2>&1; then
  die "Cannot connect to '$SSH_ALIAS' via SSH."
fi

# Ensure cache server is up
"$SCRIPT_DIR/setup_lab_cache.sh"

info "Preparing temporary baker VM $BAKER_VM_ID from template $ORIGINAL_TEMPLATE_ID..."
run_id="$(od -An -N4 -tx1 /dev/urandom | tr -d ' \n')"
KEY_FILE="/tmp/ra8_bake_$run_id"
rm -f "$KEY_FILE" "$KEY_FILE.pub"
ssh-keygen -q -t ed25519 -N '' -C "ra8-bake-$run_id" -f "$KEY_FILE"
scp -q "$KEY_FILE.pub" "$SSH_ALIAS:/tmp/ra8_bake.pub"

remote_root "$ORIGINAL_TEMPLATE_ID" "$BAKER_VM_ID" "$POOL_ID" "$STORAGE_ID" "$BRIDGE" "$GUEST_IP" "$GATEWAY" "$GUEST_USER" <<'REMOTE'
set -euo pipefail
orig="$1"; baker="$2"; pool="$3"; storage="$4"; bridge="$5"; ip="$6"; gw="$7"; user="$8"

if qm status "$baker" >/dev/null 2>&1; then
  qm stop "$baker" --timeout 10 >/dev/null 2>&1 || true
  qm set "$baker" --protection 0 >/dev/null 2>&1 || true
  qm destroy "$baker" --purge 1 >/dev/null 2>&1 || true
fi

# Clone template to temporary baker VM
qm clone "$orig" "$baker" --name "ra8-lab-baker" --pool "$pool" --storage "$storage" --full 1
qm set "$baker" --description "Temporary builder VM for baking ra8-ci devcontainer"
qm set "$baker" --net0 "virtio,bridge=$bridge,firewall=1,rate=10"
qm set "$baker" --ide2 "$storage:cloudinit"
qm set "$baker" --ipconfig0 "ip=$ip/24,gw=$gw"
qm set "$baker" --nameserver "1.1.1.1"
qm set "$baker" --ciuser "$user"
qm set "$baker" --sshkeys /tmp/ra8_bake.pub
rm -f /tmp/ra8_bake.pub

# Start baker VM
qm start "$baker"
REMOTE

SSH_OPTS=(-i "$KEY_FILE" -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=3)

info "Waiting for baker VM SSH to come online..."
ready=0
for _ in {1..60}; do
  if ssh "${SSH_OPTS[@]}" "$GUEST_USER@$GUEST_IP" true >/dev/null 2>&1; then
    ready=1
    break
  fi
  sleep 2
done
if ((!ready)); then
  die "Timed out waiting for baker VM SSH."
fi
success "Baker VM is online."

info "Packaging repository context for devcontainer build..."
temp_tar="/tmp/ra8_bake_src.tar"
python3 -c '
import os, sys, subprocess, shutil
repo_root = sys.argv[1]
tar_out = sys.argv[2]
subprocess.run(["git", "-C", repo_root, "archive", "--format=tar", "-o", tar_out, "HEAD"], check=True)
' "$REPO_ROOT" "$temp_tar"

scp "${SSH_OPTS[@]}" "$temp_tar" "$GUEST_USER@$GUEST_IP:/tmp/source.tar"
rm -f "$temp_tar"

info "Building ra8-ci:latest inside baker VM (using local apt and artifact caches)..."
ssh "${SSH_OPTS[@]}" "$GUEST_USER@$GUEST_IP" bash -s -- "$GATEWAY" <<'GUEST_BAKE'
set -euo pipefail
gateway="${1:-10.250.9.1}"

# Use local apt-cacher-ng proxy
if curl -s --connect-timeout 2 "http://$gateway:3142" >/dev/null 2>&1; then
  echo "Acquire::http::Proxy \"http://$gateway:3142\";" | sudo tee /etc/apt/apt.conf.d/01proxy >/dev/null
fi

# Expand partition
root_dev="$(findmnt -n -o SOURCE / 2>/dev/null || echo /dev/sda1)"
disk_name="$(lsblk -no PKNAME "$root_dev" 2>/dev/null || echo sda)"
sudo growpart "/dev/$disk_name" 1 2>/dev/null || true
sudo resize2fs "$root_dev" 2>/dev/null || true

# Subordinate IDs for rootless Podman
if ! grep -q "^$USER:" /etc/subuid 2>/dev/null; then
  sudo usermod --add-subuids 100000-165535 --add-subgids 100000-165535 "$USER"
fi

# Install substrate
sudo apt-get update -qq
sudo apt-get install -y -qq podman git git-lfs python3 curl ca-certificates fuse-overlayfs
sudo git lfs install --system >/dev/null 2>&1 || true

# Configure container runtime
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

# Unpack context
mkdir -p ~/ra8-bake
tar -xf /tmp/source.tar -C ~/ra8-bake
rm -f /tmp/source.tar

# Build ra8-ci:latest with root Podman so it's baked into root's container storage
cd ~/ra8-bake
sudo podman build -t ra8-ci:latest -f .devcontainer/Dockerfile .

# Cleanup
rm -rf ~/ra8-bake /tmp/*
sudo apt-get clean
sudo cloud-init clean --logs 2>/dev/null || true
sudo truncate -s 0 /etc/machine-id
GUEST_BAKE

success "ra8-ci:latest built successfully inside baker VM."

info "Sealing baker VM as new golden template $ORIGINAL_TEMPLATE_ID..."
remote_root "$ORIGINAL_TEMPLATE_ID" "$BAKER_VM_ID" "$POOL_ID" "$STORAGE_ID" <<'REMOTE'
set -euo pipefail
orig="$1"; baker="$2"; pool="$3"; storage="$4"

qm stop "$baker" --timeout 30 >/dev/null 2>&1 || true
for _ in {1..30}; do
  [[ "$(qm status "$baker" 2>/dev/null | awk '{print $2}')" == "stopped" ]] && break
  sleep 1
done

# Clean cloudinit and temporary disk state from baker VM
qm set "$baker" --delete ide2 >/dev/null 2>&1 || true
qm set "$baker" --delete ipconfig0,ciuser,sshkeys,nameserver >/dev/null 2>&1 || true

# Destroy old template
qm set "$orig" --protection 0 >/dev/null 2>&1 || true
qm destroy "$orig" --purge 1 >/dev/null 2>&1 || true

# Clone baker cleanly to golden template ID
qm clone "$baker" "$orig" --name "ra8-lab-debian-template" --pool "$pool" --storage "$storage" --full 1
qm set "$orig" --description "Pre-baked Debian 12 amd64 template with ra8-ci:latest; RA8_LAB_TEMPLATE=linux-ci-v2"
qm template "$orig"
qm set "$orig" --protection 1

# Destroy temporary baker VM
qm destroy "$baker" --purge 1 >/dev/null 2>&1 || true
REMOTE

rm -f "$KEY_FILE" "$KEY_FILE.pub"
trap - EXIT
success "Template $ORIGINAL_TEMPLATE_ID pre-baked and sealed successfully!"
