#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Disposable VM 9020 lifecycle. The existing CI recipe owns vmbr9 and its
# per-run firewall; OpenTofu owns only the guest.

set -euo pipefail
umask 077

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd -P)"
source "$REPO_ROOT/scripts/dev/proxmox_lab_ci.sh"

TF_WRAPPER="$REPO_ROOT/infra/terraform/run-with-openbao.sh"
POLICY="$REPO_ROOT/infra/terraform/lab-guest-policy.py"
STATE_DIR="${RA8_TOFU_GUEST_STATE_DIR:-${XDG_STATE_HOME:-$HOME/.local/state}/ra8-tofu-guest-9020}"
VM_ID=9020
TEMPLATE_ID=9001
BRIDGE="vmbr9"
SUBNET="10.250.9.0/24"
GATEWAY="10.250.9.1"
POOL="ra8-tf-lab"
DATASTORE="ra8-tf-lab"
NODE="pve1"
NETWORK_READY=0
API_READY=0
GUEST_SSH_READY=0
SUCCESS=0
ACTION=""
STATE_CREATED=0
api_port=""

say_error() {
  printf 'error: %s\n' "$*" >&2
  exit 1
}

policy() {
  python3 "$POLICY" "$@"
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
  LAB_HOST="10.250.9.30"
  NFT_TABLE="ra8_lab_ci_${run_id}"
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
  export TF_VAR_template_name="ra8-lab-debian-template"
  export TF_VAR_bridge="$BRIDGE"
  export TF_VAR_pool_id="$POOL"
  export TF_VAR_datastore_id="$DATASTORE"
  export TF_VAR_ipv4_address="10.250.9.30/24"
  export TF_VAR_guest_username=terraform-lab
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
  template_check "$TEMPLATE_ID" "ra8-lab-debian-template" "RA8_LAB_TEMPLATE=linux-ci-v1"
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

  start_guest_ssh_proxy guest
  GUEST_SSH_READY=1
  wait_for_guest_ssh terraform-lab
  guest_digest="$(policy check-guest "$run_id")"
  write_metadata
  [[ "$(python3 "$POLICY" digest-template)" == "$source_template_digest" ]] ||
    say_error "source template digest changed while the clone was being created"
  SUCCESS=1
  printf 'VM %s accepted the recipe SSH readiness probe on %s in pool %s; source and copied config digests are bound to this run.\n' "$VM_ID" "$BRIDGE" "$POOL"
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
  run_tofu init -reconfigure -input=false -backend-config="path=$STATE_DIR/terraform.tfstate"
  run_tofu plan -destroy -input=false -out="$STATE_DIR/destroy.tfplan"
  run_tofu apply -input=false "$STATE_DIR/destroy.tfplan"
  policy check-absent
  NETWORK_READY=1
  network_ready=1
  cleanup_lab_network
  rm -rf -- "$STATE_DIR"
  SUCCESS=1
  printf 'VM %s destroyed; its run firewall and temporary bridge were removed when no other recipe run remained.\n' "$VM_ID"
}

check_read_only() {
  [[ "$(uname -s)" == "Darwin" ]] || say_error "run this OpenTofu lifecycle only from the controller Mac"
  [[ "${RA8_LAB_NODE:-pve1}" == "$NODE" ]] || say_error "this entrypoint is allowlisted only for pve1"
  policy check-template
  printf 'VMID %s is reserved; pool/datastore %s; target bridge %s is created only by the lab recipe.\n' "$VM_ID" "$POOL" "$BRIDGE"
}

main() {
  local action="${1:-}"
  if [[ -z "${RA8_LAB_NODE:-}" && "$action" != "--selftest" ]]; then
    RA8_LAB_NODE="$(ssh -o BatchMode=yes -o RequestTTY=no -o ConnectTimeout=5 pve hostname 2>/dev/null || true)"
  fi
  case "$action" in
    --selftest)
      policy --selftest
      ;;
    check)
      check_read_only
      ;;
    create)
      check_operator_boundary
      ACTION=create
      trap finish EXIT
      trap 'exit 130' INT
      trap 'exit 143' TERM
      create_guest
      ;;
    destroy)
      ACTION=destroy
      check_operator_boundary
      trap finish EXIT
      trap 'exit 130' INT
      trap 'exit 143' TERM
      destroy_guest
      ;;
    *)
      printf 'Usage: %s {check|create|destroy|--selftest}\n' "$0" >&2
      return 2
      ;;
  esac
}

main "$@"
