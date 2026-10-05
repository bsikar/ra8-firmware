#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Disposable Linux and Windows guest lifecycle. The existing CI recipe owns
# vmbr9 and its per-run firewall; OpenTofu owns only the guest.

set -euo pipefail
umask 077

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd -P)"
source "$REPO_ROOT/scripts/dev/proxmox_lab_ci.sh"

TF_WRAPPER="$REPO_ROOT/infra/terraform/run-with-openbao.sh"
POLICY="$REPO_ROOT/infra/terraform/lab-guest-policy.py"
PROFILE="${RA8_TOFU_GUEST_PROFILE:-linux}"
STATE_DIR="${RA8_TOFU_GUEST_STATE_DIR:-${XDG_STATE_HOME:-$HOME/.local/state}/ra8-tofu-guest-${PROFILE}}"
VM_ID="${RA8_TOFU_GUEST_VM_ID:-}"
# Adapter contract: RA8_TOFU_GUEST_PROFILE=linux|windows and
# RA8_TOFU_GUEST_VM_ID=<decimal>; standalone defaults are Linux and VMID 9020.
TEMPLATE_ID=9001
TEMPLATE_NAME="ra8-lab-debian-template"
TEMPLATE_MARKER="RA8_LAB_TEMPLATE=linux-ci-v1"
DISK_SIZE_GB=32
GUEST_USERNAME=terraform-lab
GUEST_ADDRESS="10.250.9.30"
BRIDGE="vmbr9"
SUBNET="10.250.9.0/24"
GATEWAY="10.250.9.1"
POOL="ra8-tf-lab"
DATASTORE="ra8-tf-lab"
NODE="pve1"
NETWORK_READY=0
API_READY=0
GUEST_SSH_READY=0
WINRM_PROXY_PID=""
WINRM_PROXY_PORT=""
SUCCESS=0
ACTION=""
STATE_CREATED=0
api_port=""
PINNED_TOFU_BIN="$HOME/ra8ci-work/toolchain/tofu-1.13.0/tofu"
TOFU_VERSION="1.13.0"

tofu_sha256() {
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  elif command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    return 1
  fi
}

tofu_pin_for() {
  case "$1/$2" in
    Darwin/arm64)
      printf '%s\n' '632f95ef06da253a12bbaf4e1ae88718f1fb1882f407915435492b93461a6bc1'
      ;;
    *)
      return 1
      ;;
  esac
}

verify_tofu_identity() {
  local binary="$1" expected_version="$2" expected_sha="$3" actual_sha version_line
  [[ -f "$binary" && -x "$binary" && ! -L "$binary" ]] || {
    printf '%s\n' 'error: pinned OpenTofu binary is missing, not executable, or is a symlink.' >&2
    return 1
  }
  actual_sha="$(tofu_sha256 "$binary")" || {
    printf '%s\n' 'error: no supported SHA-256 utility is available.' >&2
    return 1
  }
  [[ "$actual_sha" == "$expected_sha" ]] || {
    printf '%s\n' 'error: OpenTofu binary SHA-256 does not match the platform pin.' >&2
    return 1
  }
  version_line="$("$binary" version 2>/dev/null | sed -n '1p')" || {
    printf '%s\n' 'error: pinned OpenTofu version probe failed.' >&2
    return 1
  }
  [[ "$version_line" == "OpenTofu v${expected_version}" ]] || {
    printf '%s\n' 'error: OpenTofu version does not match the required version.' >&2
    return 1
  }
}

check_pinned_tofu() {
  local os arch expected_sha
  os="$(uname -s)"
  arch="$(uname -m)"
  case "$arch" in
    aarch64) arch=arm64 ;;
    x86_64) arch=amd64 ;;
  esac
  expected_sha="$(tofu_pin_for "$os" "$arch")" ||
    say_error "OpenTofu 1.13.0 has no approved binary pin for ${os}/${arch}"
  verify_tofu_identity "$PINNED_TOFU_BIN" "$TOFU_VERSION" "$expected_sha" || exit 1
  PATH="$(dirname "$PINNED_TOFU_BIN"):$PATH"
  export PATH
}

say_error() {
  printf 'error: %s\n' "$*" >&2
  exit 1
}

policy() {
  if [[ -n "$VM_ID" ]]; then
    python3 "$POLICY" --profile "$PROFILE" --vm-id "$VM_ID" "$@"
  else
    python3 "$POLICY" --profile "$PROFILE" "$@"
  fi
}

select_profile() {
  case "$1" in
    linux)
      PROFILE=linux
      TEMPLATE_NAME=ra8-lab-debian-template TEMPLATE_MARKER=RA8_LAB_TEMPLATE=linux-ci-v1
      TEMPLATE_ID=9001 DISK_SIZE_GB=32 GUEST_USERNAME=terraform-lab
      if [[ -z "$VM_ID" ]]; then
        VM_ID=9020
      fi
      if [[ -n "$VM_ID" ]]; then
        [[ "$VM_ID" =~ ^90(2[0-9]|3[0-9])$ && "$VM_ID" != 9021 ]] ||
          say_error "Linux VMID must be a reservation from 9020-9039, excluding Windows VMID 9021"
        GUEST_ADDRESS="10.250.9.$((VM_ID - 8990))"
      fi
      ;;
    windows)
      PROFILE=windows
      [[ -z "$VM_ID" || "$VM_ID" == 9021 ]] || say_error "Windows profile is assigned VMID 9021"
      VM_ID=9021 TEMPLATE_ID=9011
      TEMPLATE_NAME=ra8-lab-windows-template TEMPLATE_MARKER=RA8_LAB_TEMPLATE=windows-ci-v1
      DISK_SIZE_GB=64 GUEST_USERNAME=Administrator GUEST_ADDRESS=10.250.9.31
      ;;
    *) say_error "guest profile must be linux or windows" ;;
  esac
  refresh_state_dir
}

refresh_state_dir() {
  if [[ -n "${RA8_TOFU_GUEST_STATE_DIR:-}" ]]; then
    STATE_DIR="$RA8_TOFU_GUEST_STATE_DIR"
  else
    STATE_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/ra8-tofu-guest-${PROFILE}-${VM_ID}"
  fi
}

check_operator_boundary() {
  [[ "$(uname -s)" == "Darwin" ]] || say_error "run this OpenTofu lifecycle only from the controller Mac"
  [[ "${RA8_LAB_NETWORK_APPROVED:-}" == "1" ]] || say_error "set RA8_LAB_NETWORK_APPROVED=1 for the reviewed temporary lab network"
  [[ "${RA8_LAB_EGRESS_APPROVED:-}" == "1" ]] || say_error "set RA8_LAB_EGRESS_APPROVED=1 for the recipe's per-run egress policy"
  [[ -x "$TF_WRAPPER" && -r "$POLICY" ]] || say_error "OpenTofu wrapper or lifecycle policy helper is unavailable"
  [[ "${RA8_LAB_NODE:-}" == "$NODE" ]] || say_error "set RA8_LAB_NODE=pve1 for this allowlisted lifecycle"
  command -v python3 >/dev/null 2>&1 || say_error "python3 is required for the read-only boundary checks"
  command -v ssh >/dev/null 2>&1 || say_error "ssh is required for the approved controller tunnel"
  command -v ssh-keygen >/dev/null 2>&1 || say_error "ssh-keygen is required for the per-run readiness key"
}

configure_network() {
  LAB_BRIDGE="$BRIDGE"
  LAB_SUBNET="$SUBNET"
  LAB_GATEWAY="$GATEWAY"
  LAB_VM_ID="$VM_ID"
  LAB_HOST="$GUEST_ADDRESS"
  NFT_TABLE="ra8_lab_ci_${run_id}"
}

start_guest_winrm_proxy() {
  WINRM_PROXY_PORT="$(local_port)"
  python3 "$REPO_ROOT/scripts/dev/ssh_loopback_proxy.py" --port "$WINRM_PROXY_PORT" --target guest_windows_winrm >/dev/null 2>&1 &
  WINRM_PROXY_PID=$!
  for _ in {1..30}; do
    kill -0 "$WINRM_PROXY_PID" 2>/dev/null || say_error "the localhost WinRM tunnel exited early"
    if (echo >/dev/tcp/127.0.0.1/"$WINRM_PROXY_PORT") >/dev/null 2>&1; then
      return
    fi
    sleep 1
  done
  say_error "the localhost WinRM tunnel did not become reachable"
}

stop_guest_winrm_proxy() {
  if [[ -n "$WINRM_PROXY_PID" ]] && kill -0 "$WINRM_PROXY_PID" 2>/dev/null; then
    kill "$WINRM_PROXY_PID" 2>/dev/null || true
    wait "$WINRM_PROXY_PID" 2>/dev/null || true
  fi
  WINRM_PROXY_PID=""
  WINRM_PROXY_PORT=""
}

check_guest_winrm() {
  local response_code
  response_code="$(python3 - "$WINRM_PROXY_PORT" <<'PY'
from http.client import HTTPConnection
import sys

connection = HTTPConnection("127.0.0.1", int(sys.argv[1]), timeout=8)
try:
    connection.request("POST", "/wsman", body=b"", headers={"Content-Type": "application/soap+xml;charset=UTF-8"})
    response = connection.getresponse()
    print(response.status)
finally:
    connection.close()
PY
)" || say_error "the Windows WinRM endpoint did not answer over the loopback proxy"
  [[ "$response_code" == 401 ]] || say_error "the Windows WinRM endpoint returned HTTP $response_code instead of the expected unauthenticated 401"
  printf 'Windows WinRM endpoint reachable through loopback proxy (HTTP %s).\n' "$response_code"
}

start_api_tunnel() {
  api_port="$(local_port)"
  start_api_proxy "$api_port"
  API_READY=1
  verify_api_identity "$api_port"
  export TF_VAR_proxmox_endpoint="https://127.0.0.1:${api_port}/"
  export TF_VAR_proxmox_insecure=true
  export TF_VAR_node_name="$NODE"
  export TF_VAR_run_id="$run_id"
  export TF_VAR_vm_id="$VM_ID"
  export TF_VAR_template_vm_id="$TEMPLATE_ID"
  export TF_VAR_template_name="$TEMPLATE_NAME"
  export TF_VAR_guest_profile="$PROFILE"
  export TF_VAR_bridge="$BRIDGE"
  export TF_VAR_pool_id="$POOL"
  export TF_VAR_datastore_id="$DATASTORE"
  export TF_VAR_ipv4_address="${GUEST_ADDRESS}/24"
  export TF_VAR_guest_username="$GUEST_USERNAME"
  export TF_VAR_disk_size_gb="$DISK_SIZE_GB"
  export TF_VAR_ssh_public_key="$(<"$STATE_DIR/id_ed25519.pub")"
  export RA8_TOFU_ENV=lab-guest
  export TF_DATA_DIR="$STATE_DIR/tf-data"
}

run_tofu() {
  "$TF_WRAPPER" "$@"
}

load_run_metadata() {
  [[ -d "$STATE_DIR" && ! -L "$STATE_DIR" && "$(stat -f '%Lp' "$STATE_DIR" 2>/dev/null || stat -c '%a' "$STATE_DIR")" == 700 && "$(stat -f '%u' "$STATE_DIR" 2>/dev/null || stat -c '%u' "$STATE_DIR")" == "$(id -u)" ]] ||
    say_error "private lifecycle state directory is unavailable"
  [[ -f "$STATE_DIR/run.env" && ! -L "$STATE_DIR/run.env" ]] || say_error "lifecycle run metadata is unavailable"
  [[ "$(stat -f '%Lp' "$STATE_DIR/run.env" 2>/dev/null || stat -c '%a' "$STATE_DIR/run.env")" == 600 ]] ||
    say_error "lifecycle run metadata permissions are not private"
  run_id="$(awk -F= '$1 == "run_id" {print $2; exit}' "$STATE_DIR/run.env")"
  source_template_digest="$(awk -F= '$1 == "template_digest" {print $2; exit}' "$STATE_DIR/run.env")"
  guest_digest="$(awk -F= '$1 == "guest_digest" {print $2; exit}' "$STATE_DIR/run.env")"
  stored_profile="$(awk -F= '$1 == "profile" {print $2; exit}' "$STATE_DIR/run.env")"
  stored_vmid="$(awk -F= '$1 == "vm_id" {print $2; exit}' "$STATE_DIR/run.env")"
  [[ "$stored_profile" == "$PROFILE" && "$stored_vmid" == "$VM_ID" ]] || say_error "lifecycle metadata does not match the selected profile"
  validate_hex_id "$run_id"
  [[ "$source_template_digest" =~ ^[0-9a-f]{40}$ && "$guest_digest" =~ ^([0-9a-f]{40})?$ ]] ||
    say_error "lifecycle metadata has an invalid config digest"
}

write_metadata() {
  local file="$STATE_DIR/run.env" temp="$STATE_DIR/run.env.tmp"
  {
    printf 'run_id=%s\n' "$run_id"
    printf 'template_digest=%s\n' "$source_template_digest"
    printf 'guest_digest=%s\n' "$guest_digest"
    printf 'profile=%s\n' "$PROFILE"
    printf 'vm_id=%s\n' "$VM_ID"
  } >"$temp"
  chmod 600 "$temp"
  mv -f -- "$temp" "$file"
}

finish() {
  local rc=$?
  trap - EXIT INT TERM
  if ((API_READY)); then
    stop_api_proxy
  fi
  if ((GUEST_SSH_READY)); then
    stop_guest_ssh_proxy
  fi
  stop_guest_winrm_proxy
  if ((rc != 0 && SUCCESS == 0 && STATE_CREATED == 1)) && [[ "$ACTION" == "create" ]]; then
    if ! policy check-absent >/dev/null 2>&1; then
      guest_digest="$(policy check-guest-destroy "$run_id" 2>/dev/null || true)"
      if [[ "$guest_digest" =~ ^[0-9a-f]{40}$ ]]; then
        write_metadata
      fi
      printf 'guest or encrypted state remains for safe recovery in %s\n' "$STATE_DIR" >&2
    else
      if ((NETWORK_READY)); then
        cleanup_lab_network || printf '%s\n' 'error: temporary lab network cleanup needs review' >&2
      fi
      rm -rf -- "$STATE_DIR"
    fi
  fi
  exit "$rc"
}

create_guest() {
  [[ ! -e "$STATE_DIR" && ! -L "$STATE_DIR" ]] || say_error "lifecycle state already exists; inspect it before reuse"
  policy check-absent >/dev/null
  run_id="$(new_run_id)"
  validate_hex_id "$run_id"
  source_template_digest=""
  guest_digest=""
  configure_network

  check_lab_bridge_absent "$BRIDGE"
  template_check "$TEMPLATE_ID" "$TEMPLATE_NAME" "$TEMPLATE_MARKER"
  policy check-template >/dev/null
  source_template_digest="$(policy digest-template)"

  mkdir -p "$(dirname "$STATE_DIR")"
  mkdir -m 700 "$STATE_DIR"
  STATE_CREATED=1
  run_dir="$STATE_DIR"
  ssh-keygen -q -t ed25519 -N '' -C "ra8-lab-$run_id" -f "$STATE_DIR/id_ed25519"
  chmod 600 "$STATE_DIR/id_ed25519"
  chmod 644 "$STATE_DIR/id_ed25519.pub"
  : >"$STATE_DIR/known_hosts"
  chmod 600 "$STATE_DIR/known_hosts"
  write_metadata
  setup_lab_network
  NETWORK_READY=1
  policy check-network "$run_id"

  start_api_tunnel
  run_tofu init -reconfigure -input=false -backend-config="path=$STATE_DIR/terraform.tfstate"
  run_tofu plan -input=false -out="$STATE_DIR/create.tfplan"
  run_tofu apply -input=false "$STATE_DIR/create.tfplan"

  if [[ "$PROFILE" == windows ]]; then
    start_guest_ssh_proxy guest_windows
  else
    start_guest_ssh_proxy guest
  fi
  GUEST_SSH_READY=1
  wait_for_guest_ssh "$GUEST_USERNAME"
  if [[ "$PROFILE" == windows ]]; then
    start_guest_winrm_proxy
    check_guest_winrm
  fi
  guest_digest="$(policy check-guest "$run_id")"
  write_metadata
  [[ "$(policy digest-template)" == "$source_template_digest" ]] ||
    say_error "source template digest changed while the clone was being created"
  SUCCESS=1
  printf "%s VM %s accepted the recipe SSH readiness probe at %s on %s in pool %s; source and copied config digests are bound to this run.\n" "$PROFILE" "$VM_ID" "$GUEST_ADDRESS" "$BRIDGE" "$POOL"
  printf 'guest_profile=%s\nguest_vmid=%s\nguest_address=%s\n' "$PROFILE" "$VM_ID" "$GUEST_ADDRESS"
  printf 'encrypted state: %s/terraform.tfstate\n' "$STATE_DIR"
  printf 'run ID: %s\n' "$run_id"
}

destroy_guest() {
  load_run_metadata
  configure_network
  policy check-network "$run_id"
  if ! policy check-absent >/dev/null 2>&1; then
    guest_digest="$(policy clear-protection "$run_id" "$guest_digest")"
    write_metadata
  fi
  start_api_tunnel
  stop_guest_before_destroy
  run_tofu init -reconfigure -input=false -backend-config="path=$STATE_DIR/terraform.tfstate"
  # The policy immediately above independently observes identity, run marker,
  # and config digest. Avoid a provider refresh here: a disconnected Windows
  # image may have no working guest agent, but its verified VM can still be
  # safely removed from the protected state.
  run_tofu plan -destroy -refresh=false -input=false -out="$STATE_DIR/destroy.tfplan"
  run_tofu apply -input=false "$STATE_DIR/destroy.tfplan"
  policy check-absent
  NETWORK_READY=1
  network_ready=1
  cleanup_lab_network
  rm -rf -- "$STATE_DIR"
  SUCCESS=1
  printf 'guest_vmid=%s\n' "$VM_ID"
  printf 'VM %s destroyed; its run firewall and temporary bridge were removed when no other recipe run remained.\n' "$VM_ID"
}

stop_guest_before_destroy() {
  local expected_name="ra8-lab-${PROFILE}-${run_id}"
  remote_root "$VM_ID" "$run_id" "$expected_name" <<'REMOTE'
set -euo pipefail
vmid="$1"
run_id="$2"
expected_name="$3"
config="$(qm config "$vmid")"
name="$(awk -F': ' '$1 == "name" {print $2; exit}' <<<"$config")"
description="$(awk -F': ' '$1 == "description" {print substr($0, index($0, ": ") + 2); exit}' <<<"$config")"
template="$(awk -F': ' '$1 == "template" {print $2; exit}' <<<"$config")"
net0="$(awk -F': ' '$1 == "net0" {print $2; exit}' <<<"$config")"
pool="$(pvesh get /cluster/resources --type vm --output-format json | jq -r --arg vmid "$vmid" '.[] | select((.vmid | tostring) == $vmid) | .pool // ""')"
[[ "$name" == "$expected_name" && "$description" == *"RA8_LAB_RUN=$run_id"* && ( -z "$template" || "$template" == 0 ) ]] || {
  printf '%s\n' 'refusing stop: VM name, run marker, or template identity differs' >&2; exit 1;
}
[[ "$net0" == *"bridge=vmbr9"* && "$pool" == "ra8-tf-lab" ]] || {
  printf '%s\n' 'refusing stop: VM bridge or pool is outside the lab boundary' >&2; exit 1;
}
while IFS= read -r line; do
  [[ -n "$line" ]] || continue
  if [[ "$line" =~ ^(scsi|virtio|sata|ide|efidisk|tpmstate|unused)[0-9]+: ]]; then
    value="${line#*: }"
    [[ "$value" == "ra8-tf-lab:"* ]] || {
      printf '%s\n' 'refusing stop: VM has a disk outside the lab datastore' >&2; exit 1;
    }
  fi
done <<<"$config"
if [[ "$(qm status "$vmid" | awk '{print $2}')" == running ]]; then
  # This VM is disposable and its verified run has ended. Abort a stuck
  # guest-agent shutdown and stop only this identity-checked reservation.
  qm stop "$vmid" --overrule-shutdown 1 --skiplock 1 --timeout 60
fi
[[ "$(qm status "$vmid" | awk '{print $2}')" == stopped ]] || {
  printf '%s\n' 'refusing destroy: verified VM did not reach stopped state' >&2; exit 1;
}
printf 'verified VM %s is stopped before destroy\n' "$vmid"
REMOTE
}

check_read_only() {
  [[ "$(uname -s)" == "Darwin" ]] || say_error "run this OpenTofu lifecycle only from the controller Mac"
  [[ "${RA8_LAB_NODE:-pve1}" == "$NODE" ]] || say_error "this entrypoint is allowlisted only for pve1"
  policy check-template
  printf 'VMID %s is reserved; pool/datastore %s; target bridge %s is created only by the lab recipe.\n' "$VM_ID" "$POOL" "$BRIDGE"
}

selftest() {
  local temp_dir fixture fixture_sha wrong_sha
  if tofu_pin_for Unsupported arch >/dev/null 2>&1; then
    say_error "OpenTofu selftest accepted an unsupported platform"
  fi
  temp_dir="$(mktemp -d)"
  fixture="$temp_dir/tofu"
  cat >"$fixture" <<'EOF'
#!/bin/sh
printf '%s\n' 'OpenTofu v1.13.0'
EOF
  chmod 700 "$fixture"
  fixture_sha="$(tofu_sha256 "$fixture")" || say_error "OpenTofu selftest could not hash its fixture"
  wrong_sha="$(printf '%064d' 0)"
  verify_tofu_identity "$fixture" "$TOFU_VERSION" "$fixture_sha" || {
    rm -rf -- "$temp_dir"
    say_error "OpenTofu selftest rejected the matching version and digest"
  }
  if verify_tofu_identity "$fixture" "$TOFU_VERSION" "$wrong_sha" >/dev/null 2>&1; then
    rm -rf -- "$temp_dir"
    say_error "OpenTofu selftest accepted a digest mismatch"
  fi
  cat >"$fixture" <<'EOF'
#!/bin/sh
printf '%s\n' 'OpenTofu v9.9.9'
EOF
  chmod 700 "$fixture"
  fixture_sha="$(tofu_sha256 "$fixture")" || say_error "OpenTofu selftest could not hash its version fixture"
  if verify_tofu_identity "$fixture" "$TOFU_VERSION" "$fixture_sha" >/dev/null 2>&1; then
    rm -rf -- "$temp_dir"
    say_error "OpenTofu selftest accepted a version mismatch"
  fi
  rm -rf -- "$temp_dir"
  case "$PROFILE" in
    linux)
      [[ "$VM_ID" =~ ^90(2[0-9]|3[0-9])$ && "$VM_ID" != 9021 && "$TEMPLATE_ID" == 9001 && "$DISK_SIZE_GB" == 32 ]] ||
        say_error "Linux profile selftest failed its VMID, template, or disk selection"
      [[ "$GUEST_ADDRESS" == "10.250.9.$((VM_ID - 8990))" ]] ||
        say_error "Linux profile selftest failed its VMID address mapping"
      ;;
    windows)
      [[ "$VM_ID" == 9021 && "$TEMPLATE_ID" == 9011 && "$DISK_SIZE_GB" == 64 && "$GUEST_ADDRESS" == 10.250.9.31 ]] ||
        say_error "Windows profile selftest failed its VMID, template, address, or disk selection"
      ;;
    *) say_error "guest profile selftest failed its allowlist" ;;
  esac
  policy --selftest
  printf 'lab-guest.sh --selftest: PASS (profile=%s, vmid=%s, template=%s)\n' "$PROFILE" "$VM_ID" "$TEMPLATE_ID"
}

main() {
  local profile="$PROFILE" action="${1:-}"
  if [[ "$action" == linux || "$action" == windows ]]; then
    profile="$action"
    action="${2:-}"
  fi
  # The selftest exercises the pin check against fixtures, so it must not
  # require the real pinned binary on the host that runs it.
  if [[ "$action" != "--selftest" ]]; then
    check_pinned_tofu
  fi
  select_profile "$profile" "$action"
  if [[ -z "${RA8_LAB_NODE:-}" && "$action" != "--selftest" ]]; then
    RA8_LAB_NODE="$(ssh -o BatchMode=yes -o RequestTTY=no -o ConnectTimeout=5 pve hostname 2>/dev/null || true)"
  fi
  case "$action" in
    --selftest)
      selftest
      ;;
    check)
      check_read_only
      ;;
    create)
      check_operator_boundary
      export RA8_TOFU_GUEST_PROFILE="$PROFILE" RA8_TOFU_GUEST_VM_ID="$VM_ID"
      ACTION=create
      trap finish EXIT
      trap 'exit 130' INT
      trap 'exit 143' TERM
      create_guest
      ;;
    destroy)
      check_operator_boundary
      export RA8_TOFU_GUEST_PROFILE="$PROFILE" RA8_TOFU_GUEST_VM_ID="$VM_ID"
      ACTION=destroy
      trap finish EXIT
      trap 'exit 130' INT
      trap 'exit 143' TERM
      destroy_guest
      ;;
    *)
      printf 'Usage: %s [linux|windows] {check|create|destroy|--selftest}\n' "$0" >&2
      return 2
      ;;
  esac
}

main "$@"
