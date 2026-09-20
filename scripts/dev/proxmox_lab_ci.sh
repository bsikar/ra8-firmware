#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Create, exercise, and remove one disposable Proxmox CI guest.
#
# This driver is intentionally narrower than the persistent fleet tooling:
# it talks to Proxmox only through the `pve` SSH alias, uses a run-local
# Terraform state file, and will destroy only a matching reserved lab guest.

set -euo pipefail
umask 077

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd -P)"
TF_ROOT="$REPO_ROOT/infra/terraform/environments/lab"
TF_WRAPPER="$REPO_ROOT/infra/terraform/run-with-openbao.sh"
PLAYBOOK="$REPO_ROOT/infra/ansible/playbooks/proxmox-lab-linux.yml"
PLAYBOOK_WINDOWS="$REPO_ROOT/infra/ansible/playbooks/proxmox-lab-windows.yml"
LOOPBACK_PROXY="$REPO_ROOT/scripts/dev/ssh_loopback_proxy.py"
SSH_ALIAS="pve"
LINUX_BRIDGE="vmbr9"
LINUX_SUBNET="10.250.9.0/24"
LINUX_GATEWAY="10.250.9.1"
LINUX_VM_ID=9000
LINUX_TEMPLATE_ID=9001
LINUX_HOST="10.250.9.10"

WINDOWS_BRIDGE="vmbr8"
WINDOWS_SUBNET="10.250.8.0/24"
WINDOWS_GATEWAY="10.250.8.1"
WINDOWS_VM_ID=9010
WINDOWS_TEMPLATE_ID=9011
WINDOWS_HOST="10.250.8.20"

LAB_BRIDGE=""
LAB_SUBNET=""
LAB_GATEWAY=""
LAB_VM_ID=0
LAB_HOST=""
NFT_TABLE=""

run_dir=""
run_id=""
proxy_pid=""
guest_proxy_pid=""
guest_ssh_port=""
network_ip_forward_before=""
network_ready=0
cleanup_enabled=0
keep_guest=0
cleanup_failed=0
ci_profile=""

usage() {
  cat <<'EOF'
scripts/dev/proxmox_lab_ci.sh -- disposable Proxmox CI lifecycle

  proxmox_lab_ci.sh check
  proxmox_lab_ci.sh ci --profile linux [--keep]
  proxmox_lab_ci.sh ci --profile windows|both
  proxmox_lab_ci.sh --selftest

The CI profile requires:
  RA8_LAB_NODE                 Proxmox node name declared for the lab
  RA8_LAB_LINUX_USER           guest user created by the Linux template
  RA8_LAB_NETWORK_APPROVED=1   explicit approval of the isolated lab bridge
  RA8_LAB_EGRESS_APPROVED=1    explicit approval of controlled guest egress

Proxmox is addressed only as the SSH alias `pve`; the Terraform API endpoint
and guest SSH endpoint are created locally as 127.0.0.1 tunnels. The guest is
always pinned to the temporary 10.250.9.0/24 lab network; no guest address is
accepted as an input.
EOF
}

die() {
  printf 'error: %s\n' "$*" >&2
  exit 1
}

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "required command is unavailable: $1"
}

validate_hex_id() {
  [[ "$1" =~ ^[0-9a-f]{16}$ ]] || die "run ID must be exactly 16 lowercase hexadecimal characters"
}

validate_user() {
  [[ "$1" =~ ^[a-z_][a-z0-9_-]{0,31}$ ]] || die "Linux guest user is not a safe account name"
}

validate_node() {
  [[ "$1" =~ ^[A-Za-z0-9][A-Za-z0-9._-]{0,62}$ ]] || die "Proxmox node name is not a safe declared name"
}

new_run_id() {
  od -An -N8 -tx1 /dev/urandom | tr -d ' \n'
}

local_port() {
  python3 - <<'PY'
import socket

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.bind(("127.0.0.1", 0))
print(sock.getsockname()[1])
sock.close()
PY
}

remote_root() {
  ssh -o BatchMode=yes -o RequestTTY=no "$SSH_ALIAS" sudo -n /bin/bash -s -- "$@"
}

template_check() {
  local template_id="$1"
  local expected_name="$2"
  local marker="$3"

  remote_root "$template_id" "$expected_name" "$marker" <<'REMOTE'
set -euo pipefail
template_id="$1"
expected_name="$2"
marker="$3"
lab_storage="ra8-tf-lab"

config="$(qm config "$template_id" 2>/dev/null)" || {
  printf 'missing template %s\n' "$template_id" >&2
  exit 1
}

name="$(awk -F': ' '$1 == "name" {print substr($0, index($0, ": ") + 2); exit}' <<<"$config")"
template="$(awk -F': ' '$1 == "template" {print $2; exit}' <<<"$config")"
description="$(awk -F': ' '$1 == "description" {print substr($0, index($0, ": ") + 2); exit}' <<<"$config")"
status="$(qm status "$template_id" | awk '{print $2}')"
pool="$(pvesh get /cluster/resources --type vm --output-format json | jq -r --arg vmid "$template_id" '.[] | select((.vmid | tostring) == $vmid) | .pool // ""')"

[[ "$name" == "$expected_name" ]] || { printf 'template %s has an unexpected name\n' "$template_id" >&2; exit 1; }
[[ "$template" == "1" ]] || { printf 'object %s is not a template\n' "$template_id" >&2; exit 1; }
[[ "$status" == "stopped" ]] || { printf 'template %s is not stopped\n' "$template_id" >&2; exit 1; }
[[ "$pool" == "ra8-tf-lab" ]] || { printf 'template %s is outside the lab pool\n' "$template_id" >&2; exit 1; }
[[ "$description" == *"$marker"* ]] || { printf 'template %s is missing its reviewed readiness marker\n' "$template_id" >&2; exit 1; }

while IFS= read -r line; do
  [[ -n "$line" ]] || continue
  if [[ "$line" =~ ^(scsi|virtio|sata|ide|efidisk|tpmstate)[0-9]*: ]]; then
    value="${line#*: }"
    [[ "$value" == "$lab_storage:"* ]] || {
      printf 'template %s has a storage volume outside the lab datastore\n' "$template_id" >&2
      exit 1
    }
  fi
done <<<"$config"
REMOTE
}

check_template_fixtures() {
  local profile="${1:-linux}"
  if [[ "$profile" == "linux" || "$profile" == "both" ]]; then
    template_check "$LINUX_TEMPLATE_ID" "ra8-lab-debian-template" "RA8_LAB_TEMPLATE=linux-ci-v1"
  fi
  if [[ "$profile" == "windows" || "$profile" == "both" ]]; then
    template_check "$WINDOWS_TEMPLATE_ID" "ra8-lab-windows-template" "RA8_LAB_TEMPLATE=windows-ci-v1"
  fi
}

check_lab_bridge_absent() {
  local bridge="${1:-$LAB_BRIDGE}"
  [[ -n "$bridge" ]] || bridge="$LINUX_BRIDGE"
  remote_root "$bridge" <<'REMOTE'
set -euo pipefail
bridge="$1"
if ip link show "$bridge" >/dev/null 2>&1; then
  if qm list 2>/dev/null | awk '$3 == "running" && ($1 == "9000" || $1 == "9010") {found=1} END {exit !found}'; then
    printf 'note: an active CI run is currently using the %s lab bridge. (Run `just infra::lab::list` to view running lab guests).\n' "$bridge" >&2
    exit 1
  else
    printf 'the reserved %s lab bridge already exists; refusing to reuse unknown host network state\n' "$bridge" >&2
    exit 1
  fi
fi
REMOTE
}

setup_lab_network() {
  NFT_TABLE="ra8_lab_ci_${run_id}"
  network_ip_forward_before="$(remote_root "$NFT_TABLE" "$LAB_GATEWAY" "$LAB_SUBNET" "$LAB_BRIDGE" "$LAB_VM_ID" <<'REMOTE'
set -euo pipefail
table="$1"
gateway="$2"
subnet="$3"
bridge="$4"
vmid="$5"

[[ ! -e /sys/class/net/"$bridge" ]] || {
  printf '%s appeared after preflight; refusing to touch it\n' "$bridge" >&2
  exit 1
}
if nft list table ip "$table" >/dev/null 2>&1; then
  printf '%s\n' 'the run-specific nft table already exists; refusing to reuse it' >&2
  exit 1
fi

old_forward="$(sysctl -n net.ipv4.ip_forward)"
rollback() {
  set +e
  nft delete table ip "$table" >/dev/null 2>&1
  ip addr flush dev "$bridge" scope global >/dev/null 2>&1
  ip link delete "$bridge" type bridge >/dev/null 2>&1
  sysctl -w "net.ipv4.ip_forward=$old_forward" >/dev/null 2>&1
}
trap rollback ERR

ip link add "$bridge" type bridge
ip addr add "${gateway}/24" dev "$bridge"
ip link set "$bridge" up
sysctl -w net.ipv4.ip_forward=1 >/dev/null
nft -f - <<EOF
table ip $table {
  set lab_ingress {
    type ifname
    elements = { "$bridge", "fwbr${vmid}i0", "fwln${vmid}i0", "fwpr${vmid}p0" }
  }

  chain input {
    type filter hook input priority -100; policy accept;
    ct state established,related accept
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
    oifname "vmbr0" ip saddr $subnet masquerade
  }
}
EOF
trap - ERR
printf '%s\n' "$old_forward"
REMOTE
)"
  network_ready=1
}

cleanup_lab_network() {
  ((network_ready)) || return 0
  remote_root "$NFT_TABLE" "$network_ip_forward_before" "$LAB_BRIDGE" <<'REMOTE'
set -euo pipefail
table="$1"
old_forward="$2"
bridge="$3"

children="$(ip -o link show master "$bridge" 2>/dev/null | awk -F': ' '{print $2}')"
[[ -z "$children" ]] || {
  printf 'refusing network cleanup; unexpected %s ports remain: %s\n' "$bridge" "$children" >&2
  exit 1
}

nft list table ip "$table" >/dev/null 2>&1 && nft delete table ip "$table"
ip addr flush dev "$bridge" scope global
ip link set "$bridge" down
ip link delete "$bridge" type bridge
[[ "$old_forward" == "0" || "$old_forward" == "1" ]] || {
  printf '%s\n' 'refusing to restore an unexpected ip_forward value' >&2
  exit 1
}
sysctl -w "net.ipv4.ip_forward=$old_forward" >/dev/null
REMOTE
  network_ready=0
}

start_api_proxy() {
  local port="$1"
  python3 "$LOOPBACK_PROXY" --port "$port" --target api >/dev/null 2>&1 &
  proxy_pid=$!
  for _ in {1..30}; do
    kill -0 "$proxy_pid" 2>/dev/null || die "the localhost Proxmox API tunnel exited early"
    if (echo >/dev/tcp/127.0.0.1/"$port") >/dev/null 2>&1; then
      return
    fi
    sleep 1
  done
  die "the localhost Proxmox API tunnel did not become reachable"
}

verify_api_identity() {
  local port="$1"
  local remote_fingerprint local_fingerprint
  remote_fingerprint="$(remote_root <<'REMOTE'
set -euo pipefail
openssl x509 -in /etc/pve/local/pve-ssl.pem -noout -fingerprint -sha256
REMOTE
  )"
  local_fingerprint="$(
    { openssl s_client -connect "127.0.0.1:${port}" -showcerts </dev/null 2>/dev/null || true; } |
      openssl x509 -noout -fingerprint -sha256
  )"
  remote_fingerprint="$(printf '%s\n' "$remote_fingerprint" | sed 's/.*=//; s/://g' | tr '[:lower:]' '[:upper:]')"
  local_fingerprint="$(printf '%s\n' "$local_fingerprint" | sed 's/.*=//; s/://g' | tr '[:lower:]' '[:upper:]')"
  [[ -n "$remote_fingerprint" && "$remote_fingerprint" == "$local_fingerprint" ]] ||
    die "the localhost API tunnel certificate did not match the certificate read through the pve SSH session"
}

stop_api_proxy() {
  if [[ -n "$proxy_pid" ]] && kill -0 "$proxy_pid" 2>/dev/null; then
    kill "$proxy_pid" 2>/dev/null || true
    wait "$proxy_pid" 2>/dev/null || true
  fi
  proxy_pid=""
}

start_guest_ssh_proxy() {
  local target="${1:-guest}"
  guest_ssh_port="$(local_port)"
  python3 "$LOOPBACK_PROXY" --port "$guest_ssh_port" --target "$target" >/dev/null 2>&1 &
  guest_proxy_pid=$!
  for _ in {1..30}; do
    kill -0 "$guest_proxy_pid" 2>/dev/null || die "the localhost guest SSH tunnel exited early"
    if (echo >/dev/tcp/127.0.0.1/"$guest_ssh_port") >/dev/null 2>&1; then
      return
    fi
    sleep 1
  done
  die "the localhost guest SSH tunnel did not become reachable"
}

stop_guest_ssh_proxy() {
  if [[ -n "$guest_proxy_pid" ]] && kill -0 "$guest_proxy_pid" 2>/dev/null; then
    kill "$guest_proxy_pid" 2>/dev/null || true
    wait "$guest_proxy_pid" 2>/dev/null || true
  fi
  guest_proxy_pid=""
}

write_inventory() {
  local target_os="${1:-linux}"
  [[ -n "$guest_ssh_port" ]] || die "guest SSH port was not assigned before writing inventory"
  local inventory="$run_dir/inventory.ini"
  local known_hosts="$run_dir/known_hosts"
  if [[ "$target_os" == "linux" ]]; then
    cat >"$inventory" <<EOF
[lab_linux]
ra8-lab-linux ansible_host=127.0.0.1 ansible_port=${guest_ssh_port} ansible_user=${RA8_LAB_LINUX_USER} ansible_private_key_file=${run_dir}/id_ed25519 ansible_python_interpreter=/usr/bin/python3

[lab_linux:vars]
ansible_host_key_checking=True
ansible_ssh_common_args='-o UserKnownHostsFile=${known_hosts} -o StrictHostKeyChecking=accept-new -o IdentitiesOnly=yes'
EOF
  elif [[ "$target_os" == "windows" ]]; then
    cat >"$inventory" <<EOF
[lab_windows]
ra8-lab-windows ansible_host=127.0.0.1 ansible_port=${guest_ssh_port} ansible_user=${RA8_LAB_WINDOWS_USER:-Administrator} ansible_private_key_file=${run_dir}/id_ed25519 ansible_connection=ssh ansible_shell_type=powershell

[lab_windows:vars]
ansible_host_key_checking=False
ansible_ssh_common_args='-o UserKnownHostsFile=${known_hosts} -o StrictHostKeyChecking=accept-new -o IdentitiesOnly=yes'
EOF
  fi
  : >"$known_hosts"
  chmod 0600 "$inventory" "$known_hosts"
}

wait_for_guest_ssh() {
  local user="$1"
  local known_hosts="$run_dir/known_hosts"
  for _ in {1..180}; do
    if ssh \
      -i "$run_dir/id_ed25519" \
      -o IdentitiesOnly=yes \
      -o BatchMode=yes \
      -o ConnectTimeout=3 \
      -o UserKnownHostsFile="$known_hosts" \
      -o StrictHostKeyChecking=accept-new \
      -p "$guest_ssh_port" \
      "${user}@127.0.0.1" \
      /bin/true >/dev/null 2>&1 || \
      ssh \
      -i "$run_dir/id_ed25519" \
      -o IdentitiesOnly=yes \
      -o BatchMode=yes \
      -o ConnectTimeout=3 \
      -o UserKnownHostsFile="$known_hosts" \
      -o StrictHostKeyChecking=accept-new \
      -p "$guest_ssh_port" \
      "${user}@127.0.0.1" \
      "powershell.exe -Command Write-Host Ready" >/dev/null 2>&1; then
      return
    fi
    sleep 2
  done
  die "Lab guest did not accept SSH connections before the timeout"
}

build_windows_tfvar() {
  local public_key
  public_key="$(<"$run_dir/id_ed25519.pub")"
  jq -cn \
    --arg run_id "$run_id" \
    --arg name "ra8-lab-win-$run_id" \
    --arg node "$RA8_LAB_NODE" \
    --arg user "${RA8_LAB_WINDOWS_USER:-Administrator}" \
    --arg public_key "$public_key" \
    '{
      enabled: true,
      name: $name,
      run_id: $run_id,
      vm_id: 9010,
      template_vm_id: 9011,
      pool_id: "ra8-tf-lab",
      datastore_id: "ra8-tf-lab",
      node_name: $node,
      cores: 4,
      memory_mb: 8192,
      bridge: "vmbr8",
      ipv4_address: "10.250.8.20/24",
      ipv4_gateway: "10.250.8.1",
      ssh_public_keys: [$public_key],
      user_name: $user,
      started: true,
      network_enabled: true
    }'
}

build_linux_tfvar() {
  local public_key
  public_key="$(<"$run_dir/id_ed25519.pub")"
  jq -cn \
    --arg run_id "$run_id" \
    --arg name "ra8-lab-linux-$run_id" \
    --arg node "$RA8_LAB_NODE" \
    --arg user "$RA8_LAB_LINUX_USER" \
    --arg public_key "$public_key" \
    '{
      enabled: true,
      name: $name,
      run_id: $run_id,
      vm_id: 9000,
      template_vm_id: 9001,
      pool_id: "ra8-tf-lab",
      datastore_id: "ra8-tf-lab",
      node_name: $node,
      cores: 4,
      memory_mb: 8192,
      bridge: "vmbr9",
      ipv4_address: "10.250.9.10/24",
      ipv4_gateway: "10.250.9.1",
      ssh_public_keys: [$public_key],
      user_name: $user,
      started: true,
      network_enabled: true
    }'
}

build_disabled_linux_tfvar() {
  jq -cn '{
    enabled: false,
    name: "",
    run_id: "",
    vm_id: null,
    template_vm_id: null,
    pool_id: "",
    datastore_id: "",
    node_name: "",
    cores: 2,
    memory_mb: 4096,
    bridge: "",
    ipv4_address: "dhcp",
    ipv4_gateway: null,
    ssh_public_keys: [],
    user_name: "terraform-lab",
    started: false,
    network_enabled: false
  }'
}

build_disabled_windows_tfvar() {
  jq -cn '{
    enabled: false,
    name: "",
    run_id: "",
    vm_id: null,
    template_vm_id: null,
    pool_id: "",
    datastore_id: "",
    node_name: "",
    cores: 4,
    memory_mb: 8192,
    bridge: "",
    ipv4_address: "dhcp",
    ipv4_gateway: null,
    ssh_public_keys: [],
    user_name: "Administrator",
    started: false,
    network_enabled: false
  }'
}

build_source_archive() {
  local source_tree="$run_dir/source-tree"
  local worktree_patch="$run_dir/worktree.patch"
  mkdir -p "$source_tree"
  (
    umask 022
    git archive --format=tar HEAD | tar -xpf - -C "$source_tree"
    # Git's foreach shell supplies archive_stage/path; they must expand there.
    # shellcheck disable=SC2016
    archive_stage="$source_tree" git -C "$REPO_ROOT" submodule foreach --recursive '
      mkdir -p "$archive_stage/$path"
      git archive --format=tar HEAD | tar -xpf - -C "$archive_stage/$path"
    '
  )
  # CI must exercise the working tree that the caller asked us to test. Apply
  # tracked edits/deletions, then add untracked first-party files without
  # copying ignored caches or credentials.
  git -C "$REPO_ROOT" diff --no-ext-diff --binary HEAD >"$worktree_patch"
  if [[ -s "$worktree_patch" ]]; then
    git -C "$source_tree" apply --binary --whitespace=nowarn "$worktree_patch"
  fi
  while IFS= read -r -d '' path; do
    mkdir -p "$source_tree/$(dirname -- "$path")"
    cp -p -- "$REPO_ROOT/$path" "$source_tree/$path"
  done < <(git -C "$REPO_ROOT" ls-files --others --exclude-standard -z)
  chmod -R u=rwX,go=rX "$source_tree"
  COPYFILE_DISABLE=1 tar --exclude='._*' -cf "$run_dir/source.tar" -C "$source_tree" .
}

cleanup_vm() {
  local vm_id="$1"
  local expected_name="$2"

  if ! remote_root "$run_id" "$vm_id" "$expected_name" <<'REMOTE'
set -euo pipefail
run_id="$1"
vm_id="$2"
expected_name="$3"

config="$(qm config "$vm_id" 2>/dev/null)" || exit 0
description="$(awk -F': ' '$1 == "description" {print substr($0, index($0, ": ") + 2); exit}' <<<"$config")"
# If this VM does not belong to this run (e.g. concurrent profile run), do not touch it
[[ "$description" == *"RA8_LAB_RUN=$run_id"* ]] || exit 0

name="$(awk -F': ' '$1 == "name" {print substr($0, index($0, ": ") + 2); exit}' <<<"$config")"
tags="$(awk -F': ' '$1 == "tags" {print $2; exit}' <<<"$config")"
template="$(awk -F': ' '$1 == "template" {print $2; exit}' <<<"$config")"

[[ "$name" == "$expected_name" ]] || { printf 'refusing cleanup: reserved VM %s has a different name\n' "$vm_id" >&2; exit 1; }
[[ -z "$tags" || ";$tags;" == *";run-$run_id;"* ]] || { printf 'refusing cleanup: reserved VM %s has no matching run tag\n' "$vm_id" >&2; exit 1; }
[[ -z "$template" || "$template" == "0" ]] || { printf 'refusing cleanup: VM %s is a template\n' "$vm_id" >&2; exit 1; }

pool="$(pvesh get /cluster/resources --type vm --output-format json | jq -r --arg vmid "$vm_id" '.[] | select((.vmid | tostring) == $vmid) | .pool // ""')"
[[ "$pool" == "ra8-tf-lab" ]] || { printf 'refusing cleanup: VM %s is outside the lab pool\n' "$vm_id" >&2; exit 1; }

while IFS= read -r line; do
  [[ -n "$line" ]] || continue
  if [[ "$line" =~ ^(scsi|virtio|sata|ide|efidisk|tpmstate)[0-9]*: ]]; then
    value="${line#*: }"
    [[ "$value" == "ra8-tf-lab:"* ]] || { printf 'refusing cleanup: VM %s has a non-lab volume\n' "$vm_id" >&2; exit 1; }
  fi
done <<<"$config"

status="$(qm status "$vm_id" | awk '{print $2}')"
if [[ "$status" == "running" ]]; then
  qm stop "$vm_id" --timeout 60
  for _ in {1..30}; do
    [[ "$(qm status "$vm_id" | awk '{print $2}')" == "stopped" ]] && break
    sleep 1
  done
fi
[[ "$(qm status "$vm_id" | awk '{print $2}')" == "stopped" ]] || {
  printf 'refusing cleanup: VM %s did not stop\n' "$vm_id" >&2
  exit 1
}

qm set "$vm_id" --protection 0 >/dev/null
qm destroy "$vm_id" --purge 1
REMOTE
  then
    cleanup_failed=1
    return 1
  fi
}

remove_state_address() {
  local address="$1"
  local state_file="$run_dir/terraform.tfstate"
  [[ -f "$state_file" ]] || return 0
  if TF_DATA_DIR="$run_dir/tf-data" terraform -chdir="$TF_ROOT" state list 2>/dev/null | grep -Fxq "$address"; then
    TF_DATA_DIR="$run_dir/tf-data" terraform -chdir="$TF_ROOT" state rm "$address" >/dev/null
  fi
}

cleanup_run() {
  stop_guest_ssh_proxy
  stop_api_proxy
  if ((keep_guest)); then
    printf 'keeping disposable lab guest for review (run %s)\n' "$run_id" >&2
    return
  fi
  if [[ "${ci_profile:-}" == "linux" || "${ci_profile:-}" == "both" || -z "${ci_profile:-}" ]]; then
    cleanup_vm "$LINUX_VM_ID" "ra8-lab-linux-$run_id" || return
    remove_state_address 'module.lab_linux_vm[0].proxmox_virtual_environment_vm.this' || cleanup_failed=1
  fi
  if [[ "${ci_profile:-}" == "windows" || "${ci_profile:-}" == "both" || -z "${ci_profile:-}" ]]; then
    cleanup_vm "$WINDOWS_VM_ID" "ra8-lab-win-$run_id" || return
    remove_state_address 'module.lab_windows_vm[0].proxmox_virtual_environment_vm.this' || cleanup_failed=1
  fi
  cleanup_lab_network || return
  if ((cleanup_failed)); then
    printf 'cleanup did not complete; refusing to remove the run directory\n' >&2
    return
  fi
  rm -rf -- "$run_dir"
}

finish() {
  local rc=$?
  trap - EXIT INT TERM
  if ((cleanup_enabled)); then
    cleanup_run || cleanup_failed=1
  else
    stop_guest_ssh_proxy
    stop_api_proxy
  fi
  if ((cleanup_failed)); then
    printf 'error: lab cleanup failed; inspect the reserved run objects before retrying\n' >&2
    ((rc == 0)) && rc=1
  fi
  exit "$rc"
}

preflight_tools() {
  local profile="${1:-linux}"
  for tool in ssh ssh-keygen ansible-playbook git jq python3 terraform security; do
    require_cmd "$tool"
  done
  [[ -x "$TF_WRAPPER" ]] || die "OpenBao Terraform wrapper is unavailable"
  if [[ "$profile" == "linux" ]]; then
    [[ -r "$PLAYBOOK" ]] || die "Linux lab Ansible playbook is unavailable"
  elif [[ "$profile" == "windows" ]]; then
    [[ -r "$PLAYBOOK_WINDOWS" ]] || die "Windows lab Ansible playbook is unavailable"
  fi
  [[ -r "$LOOPBACK_PROXY" ]] || die "localhost SSH proxy helper is unavailable"
}

preflight_local() {
  local profile="${1:-linux}"
  local bridge=""
  if [[ "$profile" == "windows" ]]; then
    bridge="$WINDOWS_BRIDGE"
  elif [[ "$profile" == "linux" ]]; then
    bridge="$LINUX_BRIDGE"
  fi
  preflight_tools "$profile" || return 1
  terraform -chdir="$TF_ROOT" validate >/dev/null || return 1
  ssh -o BatchMode=yes -o RequestTTY=no "$SSH_ALIAS" /bin/true >/dev/null || return 1
  check_lab_bridge_absent "$bridge" || return 1
  check_template_fixtures "$profile" || return 1
}

selftest() {
  validate_hex_id 0123456789abcdef
  validate_user terraform-lab
  validate_node lab-node
  if (validate_hex_id bad) >/dev/null 2>&1; then
    die "selftest accepted a malformed run ID"
  fi
  [[ "$LINUX_HOST" == "10.250.9.10" ]] || die "selftest changed the pinned lab guest address"
  [[ "$LINUX_GATEWAY" == "10.250.9.1" ]] || die "selftest changed the pinned lab gateway"
  printf '%s\n' 'proxmox_lab_ci.sh --selftest: PASS'
}

check_only() {
  local linux_out windows_out
  local linux_rc=0 windows_rc=0

  if ! linux_out="$(set -e; preflight_local "linux" 2>&1)"; then
    linux_rc=1
  fi

  if ! windows_out="$(set -e; preflight_local "windows" 2>&1)"; then
    windows_rc=1
  fi

  printf '=== Linux CI Profile ===\n'
  if ((linux_rc == 0)); then
    printf 'STATUS: READY to run.\n\n'
  else
    printf 'STATUS: BUSY / UNAVAILABLE\nReason:\n%s\n\n' "$linux_out"
  fi

  printf '=== Windows CI Profile ===\n'
  if ((windows_rc == 0)); then
    printf 'STATUS: READY to run.\n\n'
  else
    printf 'STATUS: BUSY / UNAVAILABLE\nReason:\n%s\n\n' "$windows_out"
  fi

  if ((linux_rc == 0 && windows_rc == 0)); then
    printf 'Proxmox lab preflight passed for both profiles; no guest or host state was changed.\n'
    exit 0
  else
    exit 1
  fi
}

run_ci() {
  local profile="$1"
  ci_profile="$profile"
  [[ "$profile" == "linux" || "$profile" == "windows" ]] || die "profile '$profile' must be linux or windows"
  [[ "${RA8_LAB_NETWORK_APPROVED:-}" == "1" ]] || die "set RA8_LAB_NETWORK_APPROVED=1 only after reviewing the isolated lab bridge boundary"
  [[ "${RA8_LAB_EGRESS_APPROVED:-}" == "1" ]] || die "set RA8_LAB_EGRESS_APPROVED=1 only after reviewing controlled guest package/image egress"
  if [[ -z "${RA8_LAB_NODE:-}" ]]; then
    RA8_LAB_NODE="$(ssh -o BatchMode=yes -o RequestTTY=no -o ConnectTimeout=3 "$SSH_ALIAS" hostname 2>/dev/null || echo pve1)"
  fi
  validate_node "$RA8_LAB_NODE"
  if [[ "$profile" == "linux" ]]; then
    RA8_LAB_LINUX_USER="${RA8_LAB_LINUX_USER:-terraform-lab}"
    validate_user "$RA8_LAB_LINUX_USER"
    LAB_BRIDGE="$LINUX_BRIDGE"
    LAB_SUBNET="$LINUX_SUBNET"
    LAB_GATEWAY="$LINUX_GATEWAY"
    LAB_VM_ID="$LINUX_VM_ID"
    LAB_HOST="$LINUX_HOST"
  elif [[ "$profile" == "windows" ]]; then
    RA8_LAB_WINDOWS_USER="${RA8_LAB_WINDOWS_USER:-Administrator}"
    LAB_BRIDGE="$WINDOWS_BRIDGE"
    LAB_SUBNET="$WINDOWS_SUBNET"
    LAB_GATEWAY="$WINDOWS_GATEWAY"
    LAB_VM_ID="$WINDOWS_VM_ID"
    LAB_HOST="$WINDOWS_HOST"
  fi

  preflight_local "$profile"
  run_id="$(new_run_id)"
  validate_hex_id "$run_id"
  run_dir="$(mktemp -d "${TMPDIR:-/tmp}/ra8-lab-ci.XXXXXXXX")"
  chmod 0700 "$run_dir"
  cleanup_enabled=1
  trap finish EXIT
  trap 'exit 130' INT
  trap 'exit 143' TERM

  ssh-keygen -q -t ed25519 -N '' -C "ra8-lab-$run_id" -f "$run_dir/id_ed25519"
  chmod 0600 "$run_dir/id_ed25519"
  chmod 0644 "$run_dir/id_ed25519.pub"
  build_source_archive
  setup_lab_network

  api_port="$(local_port)"
  start_api_proxy "$api_port"
  verify_api_identity "$api_port"
  export TF_VAR_proxmox_endpoint="https://127.0.0.1:${api_port}/"
  export TF_VAR_proxmox_insecure=true
  export TF_VAR_lab_enabled=true
  export TF_VAR_allow_lxc=false

  if [[ "$profile" == "linux" ]]; then
    export TF_VAR_lab_linux_vm="$(build_linux_tfvar)"
    export TF_VAR_lab_vm="$TF_VAR_lab_linux_vm"
    export TF_VAR_lab_windows_vm="$(build_disabled_windows_tfvar)"
  elif [[ "$profile" == "windows" ]]; then
    export TF_VAR_lab_windows_vm="$(build_windows_tfvar)"
    export TF_VAR_lab_linux_vm="$(build_disabled_linux_tfvar)"
    export TF_VAR_lab_vm="$TF_VAR_lab_linux_vm"
  fi

  plan_file="$run_dir/lab.tfplan"
  state_file="$run_dir/terraform.tfstate"
  export TF_DATA_DIR="$run_dir/tf-data"
  "$TF_WRAPPER" init -reconfigure -input=false -backend-config="path=$state_file"
  printf 'planning disposable %s guest for run %s\n' "$profile" "$run_id"
  "$TF_WRAPPER" plan -out="$plan_file"
  "$TF_WRAPPER" apply "$plan_file"

  if [[ "$profile" == "linux" ]]; then
    start_guest_ssh_proxy "guest"
    write_inventory "linux"
    wait_for_guest_ssh "$RA8_LAB_LINUX_USER"
    ANSIBLE_CONFIG="$REPO_ROOT/infra/ansible/ansible.cfg" \
      ANSIBLE_HOST_KEY_CHECKING=True \
      ansible-playbook \
        -i "$run_dir/inventory.ini" \
        "$PLAYBOOK" \
        -e "lab_ci_source_archive=$run_dir/source.tar" \
        -e "lab_ci_user=$RA8_LAB_LINUX_USER"
  elif [[ "$profile" == "windows" ]]; then
    start_guest_ssh_proxy "guest_windows"
    write_inventory "windows"
    wait_for_guest_ssh "$RA8_LAB_WINDOWS_USER"
    ANSIBLE_CONFIG="$REPO_ROOT/infra/ansible/ansible.cfg" \
      ANSIBLE_HOST_KEY_CHECKING=False \
      ansible-playbook \
        -i "$run_dir/inventory.ini" \
        "$PLAYBOOK_WINDOWS" \
        -e "lab_ci_source_archive=$run_dir/source.tar" \
        -e "lab_ci_user=$RA8_LAB_WINDOWS_USER"
  fi
}

main() {
  local command="${1:-}"
  case "$command" in
    --selftest)
      selftest
      return
      ;;
    check)
      [[ "$#" -eq 1 ]] || die "check takes no additional arguments"
      check_only
      return
      ;;
    ci)
      shift
      ;;
    -h | --help | '')
      usage
      return 0
      ;;
    *)
      usage >&2
      die "unknown command: $command"
      ;;
  esac

  local profile=""
  while [[ "$#" -gt 0 ]]; do
    case "$1" in
      --profile)
        [[ "$#" -ge 2 ]] || die '--profile requires linux, windows, or both'
        profile="$2"
        shift
        ;;
      --keep)
        keep_guest=1
        ;;
      -h | --help)
        usage
        return 0
        ;;
      *)
        die "unknown CI option: $1"
        ;;
    esac
    shift
  done
  [[ -n "$profile" ]] || die '--profile is required'
  run_ci "$profile"
}

main "$@"
