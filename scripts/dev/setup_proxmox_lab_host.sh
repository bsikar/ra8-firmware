#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Setup and configure a fresh Proxmox VE host for RA8 disposable CI runners.
# Prepares host packages, sysctl, pools, datastores, and provisions the
# Linux (VM 9001) and Windows (VM 9011) golden templates.

set -euo pipefail
umask 077

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

SSH_ALIAS="${SSH_ALIAS:-pve}"
POOL_ID="ra8-tf-lab"
STORAGE_ID="ra8-tf-lab"
LINUX_TEMPLATE_ID=9001
LINUX_TEMPLATE_NAME="ra8-lab-debian-template"
WINDOWS_TEMPLATE_ID=9011
WINDOWS_TEMPLATE_NAME="ra8-lab-windows-template"

die() {
  printf 'error: %s\n' "$*" >&2
  exit 1
}

info() {
  printf '\033[1;34m[INFO]\033[0m %s\n' "$*"
}

success() {
  printf '\033[1;32m[OK]\033[0m %s\n' "$*"
}

warn() {
  printf '\033[1;33m[WARN]\033[0m %s\n' "$*"
}

remote_root() {
  ssh -o BatchMode=yes -o RequestTTY=no "$SSH_ALIAS" sudo -n /bin/bash -s -- "$@"
}

check_ssh() {
  info "Checking SSH connection to Proxmox host '$SSH_ALIAS'..."
  if ! ssh -o BatchMode=yes -o RequestTTY=no "$SSH_ALIAS" /bin/true >/dev/null 2>&1; then
    die "Cannot connect to '$SSH_ALIAS' via SSH. Verify ~/.ssh/config contains Host $SSH_ALIAS."
  fi
  success "SSH connection established."
}

setup_host_prerequisites() {
  info "Configuring Proxmox host packages, sysctl, and resource pool..."
  remote_root "$POOL_ID" "$STORAGE_ID" <<'REMOTE'
set -euo pipefail
pool_id="$1"
storage_id="$2"

# 1. Host packages
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -qq -y --no-install-recommends \
  socat jq genisoimage curl wget ca-certificates >/dev/null

# 2. Kernel IP Forwarding (required for lab NAT / egress)
sysctl -w net.ipv4.ip_forward=1 >/dev/null
mkdir -p /etc/sysctl.d
cat >/etc/sysctl.d/99-ra8-lab.conf <<EOF
net.ipv4.ip_forward=1
EOF

# 3. Proxmox Resource Pool
if ! pvesh get /pools/"$pool_id" >/dev/null 2>&1; then
  pveum pool add "$pool_id" --comment "Disposable RA8 CI runner lab workloads" >/dev/null
fi

# 4. Lab Datastore Check / Guidance
if ! pvesm status | awk -v s="$storage_id" '$1 == s {found=1} END {exit !found}'; then
  # Auto-configure if VG ra8lab thinpool fleet exists
  if lvs ra8lab/fleet >/dev/null 2>&1; then
    pvesm add lvmthin "$storage_id" --vgname ra8lab --thinpool fleet --content images,rootdir
  elif lvs pve/data >/dev/null 2>&1; then
    # Fallback to default pve/data thinpool if ra8lab is absent
    pvesm add lvmthin "$storage_id" --vgname pve --thinpool data --content images,rootdir
  else
    printf 'error: Storage %s is missing in /etc/pve/storage.cfg and no suitable thinpool found.\n' "$storage_id" >&2
    exit 1
  fi
fi

# 5. Proxmox RBAC Role, API User, and ACL Permissions
if ! pveum role list | grep -qw "RA8TerraformLab"; then
  pveum role add RA8TerraformLab --privs "Datastore.AllocateSpace,Datastore.Audit,Pool.Audit,SDN.Use,VM.Allocate,VM.Audit,VM.Clone,VM.Config.CDROM,VM.Config.CPU,VM.Config.Cloudinit,VM.Config.Disk,VM.Config.HWType,VM.Config.Memory,VM.Config.Network,VM.Config.Options,VM.PowerMgmt" >/dev/null
fi

if ! pveum user list | grep -qw "terraform-lab@pve"; then
  pveum user add terraform-lab@pve --comment "RA8 Terraform lab API identity" >/dev/null
fi

node="$(hostname)"
for target in "terraform-lab@pve" "terraform-lab@pve!lab"; do
  pveum acl modify "/nodes/$node" -"$([[ $target == *!* ]] && echo token || echo user)" "$target" -role PVEAuditor >/dev/null 2>&1 || true
  pveum acl modify "/pool/$pool_id" -"$([[ $target == *!* ]] && echo token || echo user)" "$target" -role RA8TerraformLab >/dev/null 2>&1 || true
  pveum acl modify "/storage/$storage_id" -"$([[ $target == *!* ]] && echo token || echo user)" "$target" -role RA8TerraformLab >/dev/null 2>&1 || true
  pveum acl modify "/sdn/zones/localnetwork/vmbr8" -"$([[ $target == *!* ]] && echo token || echo user)" "$target" -role RA8TerraformLab >/dev/null 2>&1 || true
  pveum acl modify "/sdn/zones/localnetwork/vmbr9" -"$([[ $target == *!* ]] && echo token || echo user)" "$target" -role RA8TerraformLab >/dev/null 2>&1 || true
done
REMOTE
  success "Proxmox host prerequisites, pool '$POOL_ID', and RBAC permissions configured."
}

is_template_valid() {
  local vmid="$1"
  local expected_name="$2"
  local marker="$3"

  local status
  status="$(remote_root "$vmid" "$expected_name" "$marker" <<'REMOTE' 2>/dev/null || true
set -euo pipefail
vmid="$1"
expected_name="$2"
marker="$3"

config="$(qm config "$vmid" 2>/dev/null)" || exit 1
name="$(awk -F': ' '$1 == "name" {print substr($0, index($0, ": ") + 2); exit}' <<<"$config")"
template="$(awk -F': ' '$1 == "template" {print $2; exit}' <<<"$config")"
description="$(awk -F': ' '$1 == "description" {print substr($0, index($0, ": ") + 2); exit}' <<<"$config")"
status="$(qm status "$vmid" 2>/dev/null | awk '{print $2}')"

if [[ "$name" == "$expected_name" && "$template" == "1" && "$status" == "stopped" && "$description" == *"$marker"* ]]; then
  echo "valid"
else
  echo "invalid"
fi
REMOTE
)"
  [[ "$status" == "valid" ]]
}

provision_linux_template() {
  info "Checking Debian Linux CI template (VM $LINUX_TEMPLATE_ID)..."
  if is_template_valid "$LINUX_TEMPLATE_ID" "$LINUX_TEMPLATE_NAME" "RA8_LAB_TEMPLATE=linux-ci-v1"; then
    success "Debian Linux template (VM $LINUX_TEMPLATE_ID) is already present and validated."
    return 0
  fi

  info "Building Debian Linux cloud template (VM $LINUX_TEMPLATE_ID)..."
  bash "$SCRIPT_DIR/prepare_proxmox_lab_linux_template.sh"
  success "Debian Linux template (VM $LINUX_TEMPLATE_ID) successfully provisioned."
}

provision_windows_template() {
  info "Checking Windows Server 2025 CI template (VM $WINDOWS_TEMPLATE_ID)..."
  if is_template_valid "$WINDOWS_TEMPLATE_ID" "$WINDOWS_TEMPLATE_NAME" "RA8_LAB_TEMPLATE=windows-ci-v1"; then
    success "Windows Server template (VM $WINDOWS_TEMPLATE_ID) is already present and validated."
    return 0
  fi

  info "Building Windows Server 2025 template (VM $WINDOWS_TEMPLATE_ID)..."
  bash "$SCRIPT_DIR/prepare_proxmox_lab_windows_template.sh"
  success "Windows Server 2025 template (VM $WINDOWS_TEMPLATE_ID) successfully provisioned."
}

run_verification_checks() {
  info "Running lab preflight verification checks..."
  bash "$SCRIPT_DIR/proxmox_lab_ci.sh" check
  success "All Proxmox lab runner fixtures verified and ready for CI!"
}

usage() {
  cat <<EOF
Usage: $0 [all|host|linux|windows|check]

Commands:
  all       Configure host, storage/pool, install Linux & Windows templates, and verify (default)
  host      Configure Proxmox host packages, sysctl, pool, and datastore only
  linux     Ensure host is configured and build/verify Linux template (VM 9001)
  windows   Ensure host is configured and build/verify Windows template (VM 9011)
  check     Run read-only preflight verification checks against templates and network
EOF
}

main() {
  local target="${1:-all}"
  check_ssh

  case "$target" in
    all)
      setup_host_prerequisites
      provision_linux_template
      provision_windows_template
      run_verification_checks
      ;;
    host)
      setup_host_prerequisites
      ;;
    linux)
      setup_host_prerequisites
      provision_linux_template
      ;;
    windows)
      setup_host_prerequisites
      provision_windows_template
      ;;
    check)
      run_verification_checks
      ;;
    -h|--help|help)
      usage
      ;;
    *)
      usage >&2
      die "Unknown target: $target"
      ;;
  esac
}

main "$@"
