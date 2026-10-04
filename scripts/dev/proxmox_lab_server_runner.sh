#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Server-side Proxmox Lab CI runner.
# Runs directly on the Proxmox VE host (pve) in the background.
# Manages the isolated VM lifecycle, network boundaries, and streams live metrics.

set -euo pipefail
umask 077

runner_linux_guest_ip() {
  local vmid="$1"
  [[ "$vmid" =~ ^90[0-9]{2}$ ]] && ((vmid >= 9000 && vmid <= 9099)) &&
    ((vmid != 9001 && vmid != 9010 && vmid != 9011)) || return 1
  printf '10.250.9.%s\n' "$((vmid - 9000 + 10))"
}

runner_run_dir() { printf '/var/lib/ra8-lab/%s/%s\n' "$1" "$2"; }
runner_log_dir() { printf '/var/log/ra8-lab/%s\n' "$1"; }
runner_run_file() { printf '%s/%s.%s\n' "$(runner_log_dir "$1")" "$2" "$3"; }
runner_lock_file() { printf '/var/lock/ra8-lab-%s-%s.lock\n' "$1" "$2"; }

shared_network_marker_action() {
  local state_dir="$1" state="$2"
  if [[ -e "$state_dir" || -L "$state_dir" ]]; then
    [[ -e "$state" || -L "$state" ]] || return 1
    printf 'validate\n'
  else
    [[ ! -e "$state" && ! -L "$state" ]] || return 1
    printf 'adopt\n'
  fi
}

validate_shared_linux_marker() {
  local state_dir="$1" state="$2" expected_uid="$3" expected_gateway="$4" expected_subnet="$5"
  python3 - "$state_dir" "$state" "$expected_uid" "$expected_gateway" "$expected_subnet" <<'PY'
import os
import stat
import sys

state_dir, state, uid_text, gateway, subnet = sys.argv[1:]
expected_uid = int(uid_text)

try:
    directory = os.lstat(state_dir)
    marker = os.lstat(state)
    if not (
        stat.S_ISDIR(directory.st_mode)
        and directory.st_uid == expected_uid
        and stat.S_IMODE(directory.st_mode) == 0o700
        and stat.S_ISREG(marker.st_mode)
        and marker.st_uid == expected_uid
        and stat.S_IMODE(marker.st_mode) == 0o600
    ):
        raise ValueError("unsafe marker path metadata")
    descriptor = os.open(state, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
    with os.fdopen(descriptor, "r", encoding="ascii") as marker:
        fields = marker.read().split()
    if len(fields) != 3:
        raise ValueError("marker must contain exactly three fields")
    old_forward, saved_gateway, saved_subnet = fields
    if old_forward not in {"0", "1"} or saved_gateway != gateway or saved_subnet != subnet:
        raise ValueError("marker does not match the managed lab network")
    print(old_forward)
except (OSError, ValueError, UnicodeError):
    raise SystemExit(1)
PY
}

legacy_bridge_adoptable() {
  local vmid="$1" run_id="$2" vm_status="$3" vm_name="$4" description="$5"
  local tags="$6" pool="$7" template="$8" net0="$9" bridge_address="${10}"
  local ipconfig0="${11}" bridge_type="${12}" nft_tables="${13}" bridge_ports="${14}" forward="${15}"
  local tag network_option
  local -a tag_array=() network_array=() ipconfig_array=()
  [[ "$vmid" =~ ^90[0-9]{2}$ ]] && ((vmid >= 9000 && vmid <= 9099)) &&
    ((vmid != 9001 && vmid != 9010 && vmid != 9011)) || return 1
  [[ "$run_id" =~ ^[0-9a-f]{16}$ && "$vm_status" == running &&
     "$vm_name" == "ra8-lab-linux-$run_id" &&
     "$description" == "Disposable RA8 lab VM; RA8_LAB_RUN=$run_id" &&
     "$pool" == ra8-tf-lab && ( -z "$template" || "$template" == 0 ) &&
     "$bridge_address" == 10.250.9.1/24 && "$bridge_type" == bridge &&
     "$nft_tables" == "ra8_lab_ci_$run_id" && "$bridge_ports" == "tap${vmid}i0" &&
     "$forward" == 1 ]] || return 1

  local found_lab=0 found_run=0
  IFS=',;' read -r -a tag_array <<<"$tags"
  for tag in "${tag_array[@]}"; do
    [[ "$tag" == ra8-lab ]] && found_lab=1
    [[ "$tag" == "run-$run_id" ]] && found_run=1
  done
  ((found_lab && found_run)) || return 1

  IFS=',' read -r -a network_array <<<"$net0"
  for network_option in "${network_array[@]}"; do
    [[ "$network_option" == bridge=vmbr9 ]] || continue
    IFS=',' read -r -a ipconfig_array <<<"$ipconfig0"
    local expected_guest_ip="10.250.9.$((vmid - 9000 + 10))" guest_address="" guest_gateway=""
    for network_option in "${ipconfig_array[@]}"; do
      [[ "$network_option" == ip=* ]] && guest_address="${network_option#ip=}"
      [[ "$network_option" == gw=* ]] && guest_gateway="${network_option#gw=}"
    done
    [[ "$guest_address" == "$expected_guest_ip/24" && "$guest_gateway" == 10.250.9.1 ]]
    return $?
  done
  return 1
}

adopt_legacy_shared_linux_network() {
  local state_dir="$1" state="$2" bridge="$3" gateway="$4" subnet="$5"
  local current_forward bridge_address bridge_type bridge_ports nft_tables vmid run_id
  local config status name description tags pool template net0 ipconfig0 config_value
  local -a bridge_addresses=()
  [[ ! -e "$state_dir" && ! -L "$state_dir" && ! -e "$state" && ! -L "$state" ]] || return 1
  # The legacy runner enabled forwarding without recording its prior value.
  # Adoption is limited to the controller's already-enabled configured state,
  # which is saved as 1 so final cleanup preserves that baseline.
  current_forward="$(sysctl -n net.ipv4.ip_forward)"
  [[ "$current_forward" == 1 ]] || return 1
  mapfile -t bridge_addresses < <(ip -o -4 addr show dev "$bridge" | awk '{print $4}')
  [[ "${#bridge_addresses[@]}" == 1 ]] || return 1
  bridge_address="${bridge_addresses[0]}"
  bridge_type="$(ip -j -d link show dev "$bridge" | jq -r '.[0].linkinfo.info_kind // ""')"
  [[ "$bridge_type" == bridge ]] || return 1
  bridge_ports="$(ip -o link show master "$bridge" | awk -F': ' '{name=$2; sub(/@.*/, "", name); print name}')"
  [[ "$bridge_ports" =~ ^tap(90[0-9]{2})i0$ ]] || return 1
  vmid="${BASH_REMATCH[1]}"
  [[ "$vmid" != 9001 && "$vmid" != 9010 && "$vmid" != 9011 ]] || return 1

  nft_tables="$(nft list tables | awk '$1 == "table" && $2 == "ip" && $3 ~ /^ra8_lab_ci_[0-9a-f]+$/ {print $3}')"
  [[ "$(printf '%s\n' "$nft_tables" | wc -l | tr -d ' ')" == 1 ]] || return 1
  run_id="${nft_tables#ra8_lab_ci_}"
  [[ "$run_id" =~ ^[0-9a-f]{16}$ ]] || return 1

  config="$(qm config "$vmid")" || return 1
  status="$(qm status "$vmid" | awk '{print $2}')"
  config_value() { awk -F': ' -v key="$1" '$1 == key {print substr($0, index($0, ": ") + 2); exit}' <<<"$config"; }
  name="$(config_value name)"
  description="$(config_value description)"
  tags="$(config_value tags)"
  template="$(config_value template)"
  net0="$(config_value net0)"
  ipconfig0="$(config_value ipconfig0)"
  pool="$(pvesh get /cluster/resources --type vm --output-format json | jq -r --arg vmid "$vmid" '.[] | select((.vmid | tostring) == $vmid) | .pool // ""')"
  legacy_bridge_adoptable "$vmid" "$run_id" "$status" "$name" "$description" \
    "$tags" "$pool" "$template" "$net0" "$bridge_address" "$ipconfig0" "$bridge_type" \
    "$nft_tables" "$bridge_ports" "$current_forward" || return 1

  install -d -m 700 "$state_dir"
  local temp_state
  temp_state="$(mktemp "$state_dir/vmbr9.state.XXXXXX")"
  printf '%s %s %s\n' "$current_forward" "$gateway" "$subnet" >"$temp_state"
  chmod 600 "$temp_state"
  mv -- "$temp_state" "$state"
  printf '%s\n' "$current_forward"
}

runner_selftest() {
  [[ "$(runner_linux_guest_ip 9000)" == 10.250.9.10 ]] || return 1
  [[ "$(runner_linux_guest_ip 9002)" == 10.250.9.12 ]] || return 1
  [[ "$(runner_linux_guest_ip 9099)" == 10.250.9.109 ]] || return 1
  for vmid in 9001 9010 9011 9100; do
    runner_linux_guest_ip "$vmid" >/dev/null 2>&1 && return 1
  done
  [[ "$(runner_run_dir linux aaaaaaaaaaaaaaaa)" != "$(runner_run_dir linux bbbbbbbbbbbbbbbb)" ]] || return 1
  for suffix in log status pid; do
    [[ "$(runner_run_file linux aaaaaaaaaaaaaaaa "$suffix")" != "$(runner_run_file linux bbbbbbbbbbbbbbbb "$suffix")" ]] || return 1
  done
  [[ "$(runner_lock_file linux 9000)" != "$(runner_lock_file linux 9002)" ]] || return 1
  local marker_dir marker_file owner
  marker_dir="$(mktemp -d)" || return 1
  marker_file="$marker_dir/vmbr9.state"
  owner="$(id -u)"
  chmod 700 "$marker_dir"
  printf '0 10.250.9.1 10.250.9.0/24\n' >"$marker_file"
  chmod 600 "$marker_file"
  [[ "$(validate_shared_linux_marker "$marker_dir" "$marker_file" "$owner" 10.250.9.1 10.250.9.0/24)" == 0 ]] || {
    rm -rf -- "$marker_dir"
    return 1
  }
  chmod 620 "$marker_file"
  if validate_shared_linux_marker "$marker_dir" "$marker_file" "$owner" 10.250.9.1 10.250.9.0/24 >/dev/null 2>&1; then
    rm -rf -- "$marker_dir"
    return 1
  fi
  chmod 600 "$marker_file"
  chmod 720 "$marker_dir"
  if validate_shared_linux_marker "$marker_dir" "$marker_file" "$owner" 10.250.9.1 10.250.9.0/24 >/dev/null 2>&1; then
    rm -rf -- "$marker_dir"
    return 1
  fi
  rm -rf -- "$marker_dir"
  marker_dir="$(mktemp -d)" || return 1
  marker_file="$marker_dir/vmbr9.state"
  [[ "$(shared_network_marker_action "$marker_dir/absent" "$marker_dir/absent/vmbr9.state")" == adopt ]] || {
    rm -rf -- "$marker_dir"
    return 1
  }
  mkdir "$marker_dir/managed"
  printf '1 10.250.9.1 10.250.9.0/24\n' >"$marker_dir/managed/vmbr9.state"
  [[ "$(shared_network_marker_action "$marker_dir/managed" "$marker_dir/managed/vmbr9.state")" == validate ]] || {
    rm -rf -- "$marker_dir"
    return 1
  }
  rm "$marker_dir/managed/vmbr9.state"
  if shared_network_marker_action "$marker_dir/managed" "$marker_dir/managed/vmbr9.state" >/dev/null 2>&1; then
    rm -rf -- "$marker_dir"
    return 1
  fi
  rm -rf -- "$marker_dir"
  legacy_bridge_adoptable 9000 0d24d95baf22ff15 running \
    ra8-lab-linux-0d24d95baf22ff15 \
    'Disposable RA8 lab VM; RA8_LAB_RUN=0d24d95baf22ff15' \
    'terraform;ra8-lab;run-0d24d95baf22ff15' ra8-tf-lab 0 \
    'virtio=02:00:00:00:00:01,bridge=vmbr9,firewall=0' \
    10.250.9.1/24 'ip=10.250.9.10/24,gw=10.250.9.1' bridge \
    ra8_lab_ci_0d24d95baf22ff15 tap9000i0 1 || return 1
  if legacy_bridge_adoptable 9000 0d24d95baf22ff15 running \
    ra8-lab-linux-0d24d95baf22ff15 \
    'Disposable RA8 lab VM; RA8_LAB_RUN=0d24d95baf22ff15' \
    'terraform;ra8-lab;run-0d24d95baf22ff15' ra8-tf-lab 0 \
    'virtio=02:00:00:00:00:01,bridge=vmbr9,firewall=0' \
    10.250.9.1/24 'ip=10.250.9.10/24,gw=10.250.9.1' bridge \
    ra8_lab_ci_0d24d95baf22ff15 'tap9000i0 tap9002i0' 1; then
    return 1
  fi
  if legacy_bridge_adoptable 9000 0d24d95baf22ff15 running \
    ra8-lab-linux-0d24d95baf22ff15 \
    'Disposable RA8 lab VM; RA8_LAB_RUN=0d24d95baf22ff15' \
    'terraform;ra8-lab;run-0d24d95baf22ff15' ra8-tf-lab 0 \
    'virtio=02:00:00:00:00:01,bridge=vmbr9,firewall=0' \
    10.250.9.1/24 'ip=10.250.9.11/24,gw=10.250.9.1' bridge \
    ra8_lab_ci_0d24d95baf22ff15 tap9000i0 1; then
    return 1
  fi
  if legacy_bridge_adoptable 9000 0d24d95baf22ff15 running \
    ra8-lab-linux-0d24d95baf22ff15 \
    'Disposable RA8 lab VM; RA8_LAB_RUN=0d24d95baf22ff15' \
    'terraform;ra8-lab;run-0d24d95baf22ff15' ra8-tf-lab 0 \
    'virtio=02:00:00:00:00:01,bridge=vmbr9,firewall=0' \
    10.250.9.2/24 'ip=10.250.9.10/24,gw=10.250.9.1' bridge \
    ra8_lab_ci_0d24d95baf22ff15 tap9000i0 1; then
    return 1
  fi
  printf '%s\n' 'proxmox_lab_server_runner.sh --selftest: PASS'
}

if [[ "${1:-}" == --selftest ]]; then
  runner_selftest
  exit $?
fi

PROFILE="${1:-linux}"
RUN_ID="${2:-$(od -An -N8 -tx1 /dev/urandom 2>/dev/null | tr -d ' \n')}"
ARCHIVE_PATH="${3:-}"
KEEP="${4:-false}"
REQUESTED_VM_ID="${5:-}"
REQUESTED_GUEST_IP="${6:-}"

export PATH="/usr/sbin:/usr/bin:/sbin:/bin:$PATH"

if [[ "$PROFILE" != linux && "$PROFILE" != windows ]]; then
  echo "error: unknown profile '$PROFILE'" >&2
  exit 2
fi
if [[ ! "$RUN_ID" =~ ^[0-9a-f]{16}$ ]]; then
  echo "error: run ID must be exactly 16 lowercase hexadecimal characters" >&2
  exit 2
fi
if [[ "$KEEP" != true && "$KEEP" != false ]]; then
  echo "error: keep must be true or false" >&2
  exit 2
fi

ts() { date +"[%Y-%m-%d %H:%M:%S]"; }

# The Windows lab credential does not travel in the environment. It lives in a
# root-owned file on this host, is checked for presence only, and is read at the
# point of use by the process that needs it. The variable below names a PATH and
# never a value.
WINDOWS_CREDENTIAL_FILE="${RA8_LAB_WINDOWS_CREDENTIAL_FILE:-/etc/ra8-lab/windows-password}"

# Reports presence as a boolean and nothing else: the file must exist, be a
# regular non-empty file owned by the user running this script (root on the
# Proxmox host), and be unreadable by group and other. The contents are never
# read here, so no caller can turn a presence check into a disclosure.
windows_credential_present() {
  local file="$1" mode
  [[ -f "$file" ]] || return 1
  [[ -s "$file" ]] || return 1
  [[ -O "$file" ]] || return 1
  mode="$(stat -c '%a' "$file" 2>/dev/null)" || return 1
  [[ "$mode" =~ ^[0-7]{3,4}$ ]] || return 1
  if ((8#$mode & 8#077)); then
    return 1
  fi
  return 0
}

echo "$(ts) Initializing background execution for profile '$PROFILE' (PID: $$)..."

if [[ "$PROFILE" == "linux" ]]; then
  REQUESTED_VM_ID="${REQUESTED_VM_ID:-9000}"
  if [[ ! "$REQUESTED_VM_ID" =~ ^90[0-9]{2}$ ]] ||
     ((REQUESTED_VM_ID < 9000 || REQUESTED_VM_ID > 9099 || REQUESTED_VM_ID == 9001 || REQUESTED_VM_ID == 9010 || REQUESTED_VM_ID == 9011)); then
    echo "$(ts) error: Linux VMID is outside the disposable guest allowlist."
    exit 2
  fi
  VM_ID="$REQUESTED_VM_ID"
  TEMPLATE_ID=9001
  BRIDGE="vmbr9"
  SUBNET="10.250.9.0/24"
  GATEWAY="10.250.9.1"
  GUEST_IP="$(runner_linux_guest_ip "$VM_ID")" || {
    echo "$(ts) error: Linux VMID is outside the disposable guest allowlist."
    exit 2
  }
  if [[ -n "$REQUESTED_GUEST_IP" && "$REQUESTED_GUEST_IP" != "$GUEST_IP" ]]; then
    echo "$(ts) error: requested guest address does not match VMID $VM_ID."
    exit 2
  fi
  GUEST_USER="terraform-lab"
elif [[ "$PROFILE" == "windows" ]]; then
  REQUESTED_VM_ID="${REQUESTED_VM_ID:-9010}"
  [[ "$REQUESTED_VM_ID" == 9010 && ( -z "$REQUESTED_GUEST_IP" || "$REQUESTED_GUEST_IP" == "10.250.8.20" ) ]] || {
    echo "$(ts) error: Windows runner target must remain VMID 9010 at 10.250.8.20."
    exit 2
  }
  VM_ID=9010
  TEMPLATE_ID=9011
  BRIDGE="vmbr8"
  SUBNET="10.250.8.0/24"
  GATEWAY="10.250.8.1"
  GUEST_IP="10.250.8.20"
  GUEST_USER="Administrator"
  if [[ -n "${RA8_LAB_WINDOWS_PASSWORD:-}" ]]; then
    echo "$(ts) error: RA8_LAB_WINDOWS_PASSWORD is set in this runner's environment."
    echo "$(ts) A credential in the environment is already readable from the process table and any crash dump."
    echo "$(ts) Unset it and place the value in $WINDOWS_CREDENTIAL_FILE instead (root-owned, mode 0600)."
    exit 1
  fi
  if ! windows_credential_present "$WINDOWS_CREDENTIAL_FILE"; then
    echo "$(ts) error: Windows lab credential not present."
    echo "$(ts) Expected a non-empty regular file at $WINDOWS_CREDENTIAL_FILE, owned by this user, mode 0600."
    exit 1
  fi
  echo "$(ts) Windows lab credential present. Value not read at this point and never logged."
fi

NFT_TABLE="ra8_lab_ci_${RUN_ID}"

derive_uplink_from_route() {
  local route="$1" interface="" found=0 index
  local -a fields=()
  read -r -a fields <<<"$route"
  for ((index = 0; index < ${#fields[@]}; index++)); do
    if [[ "${fields[index]}" == "dev" ]]; then
      ((found == 0 && index + 1 < ${#fields[@]})) || return 1
      interface="${fields[index + 1]}"
      found=1
    fi
  done
  [[ "$interface" =~ ^[[:alnum:]_.:-]{1,15}$ ]] || return 1
  printf '%s\n' "$interface"
}

NETWORK_READY=0
VM_CREATED=0
setup_shared_linux_network() {
  exec 8>/run/lock/ra8-lab-ci-vmbr9.lock
  flock -x 8
  local state_dir=/run/ra8-lab-ci state=/run/ra8-lab-ci/vmbr9.state
  local old_forward="" route uplink temp_state bridge_created=0
  route="$(ip -o route get 1.1.1.1)"
  uplink="$(derive_uplink_from_route "$route")" || { echo "$(ts) error: unsafe uplink route: $route"; return 1; }
  if nft list table ip "$NFT_TABLE" >/dev/null 2>&1; then
    echo "$(ts) error: run firewall table already exists: $NFT_TABLE"
    return 1
  fi
  if ip link show "$BRIDGE" >/dev/null 2>&1; then
    case "$(shared_network_marker_action "$state_dir" "$state")" in
      validate)
        old_forward="$(validate_shared_linux_marker "$state_dir" "$state" 0 "$GATEWAY" "$SUBNET")" || {
          echo "$(ts) error: vmbr9 lifecycle marker is mismatched or unsafe"; return 1;
        }
        ;;
      adopt)
        old_forward="$(adopt_legacy_shared_linux_network "$state_dir" "$state" "$BRIDGE" "$GATEWAY" "$SUBNET")" || {
          echo "$(ts) error: refusing to adopt vmbr9 without the exact legacy lab guest topology"; return 1;
        }
        echo "$(ts) Adopted the validated legacy vmbr9 run; preserving ip_forward=$old_forward as its restore baseline."
        ;;
      *)
        echo "$(ts) error: vmbr9 lifecycle state is incomplete or unsafe"; return 1;
        ;;
    esac
    [[ "$(ip -o -4 addr show dev "$BRIDGE" | awk 'NR == 1 {print $4}')" == "$GATEWAY/24" &&
       "$(sysctl -n net.ipv4.ip_forward)" == 1 ]] || {
      echo "$(ts) error: vmbr9 does not match the managed lab network"; return 1;
    }
  else
    [[ ! -e "$state" && ! -L "$state" ]] || { echo "$(ts) error: stale vmbr9 lifecycle marker"; return 1; }
    if [[ -e "$state_dir" || -L "$state_dir" ]]; then
      [[ -d "$state_dir" && ! -L "$state_dir" && "$(stat -c %u "$state_dir")" == 0 && "$(stat -c %a "$state_dir")" == 700 &&
         -z "$(find "$state_dir" -mindepth 1 -maxdepth 1 -print -quit)" ]] || {
        echo "$(ts) error: vmbr9 lifecycle directory is unsafe or nonempty"; return 1;
      }
    else
      install -d -m 700 "$state_dir"
    fi
    old_forward="$(sysctl -n net.ipv4.ip_forward)"
    [[ "$old_forward" == 0 || "$old_forward" == 1 ]] || { echo "$(ts) error: unexpected ip_forward value"; return 1; }
    temp_state="$(mktemp "$state_dir/vmbr9.state.XXXXXX")"
    printf '%s %s %s\n' "$old_forward" "$GATEWAY" "$SUBNET" >"$temp_state"
    chmod 600 "$temp_state"
    mv -- "$temp_state" "$state"
    if ! ip link add "$BRIDGE" type bridge; then
      rm -f -- "$state"
      return 1
    fi
    bridge_created=1
    ip addr add "$GATEWAY/24" dev "$BRIDGE"
    ip link set "$BRIDGE" up
    sysctl -w net.ipv4.ip_forward=1 >/dev/null
  fi
  NETWORK_READY=1
  if ! nft -f - <<EOF
table ip $NFT_TABLE {
  set lab_ingress { type ifname; elements = { "$BRIDGE", "fwbr${VM_ID}i0", "fwln${VM_ID}i0", "fwpr${VM_ID}p0" } }
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
    iifname @lab_ingress oifname "$uplink" udp dport 53 accept
    iifname @lab_ingress oifname "$uplink" tcp dport 53 accept
    iifname @lab_ingress oifname "$uplink" tcp dport { 80, 443 } accept
    iifname @lab_ingress drop
  }
  chain postrouting {
    type nat hook postrouting priority srcnat; policy accept;
    oifname "$uplink" ip saddr $SUBNET masquerade
  }
}
EOF
  then
    if ((bridge_created)); then
      ip addr flush dev "$BRIDGE" scope global >/dev/null 2>&1 || true
      ip link delete "$BRIDGE" type bridge >/dev/null 2>&1 || true
      sysctl -w "net.ipv4.ip_forward=$old_forward" >/dev/null 2>&1 || true
      rm -f -- "$state"
    fi
    NETWORK_READY=0
    flock -u 8
    return 1
  fi
  flock -u 8
}

cleanup_shared_linux_network() {
  ((NETWORK_READY)) || return 0
  exec 8>/run/lock/ra8-lab-ci-vmbr9.lock
  flock -x 8
  local state_dir=/run/ra8-lab-ci state=/run/ra8-lab-ci/vmbr9.state
  local old_forward remaining_tables children
  old_forward="$(validate_shared_linux_marker "$state_dir" "$state" 0 "$GATEWAY" "$SUBNET")" || {
    echo "$(ts) error: refusing vmbr9 cleanup without its trusted marker"; return 1;
  }
  nft delete table ip "$NFT_TABLE" >/dev/null 2>&1 || true
  remaining_tables="$(nft list tables | awk -v current="$NFT_TABLE" '$1 == "table" && $2 == "ip" && $3 ~ /^ra8_lab_ci_[0-9a-f]{16}$/ && $3 != current {print $3}')"
  children="$(ip -o link show master "$BRIDGE" 2>/dev/null | awk -F': ' '{print $2}')"
  if [[ -n "$remaining_tables" || -n "$children" ]]; then
    echo "$(ts) Preserving shared $BRIDGE; other run tables or guest ports remain."
    NETWORK_READY=0
    flock -u 8
    return 0
  fi
  ip addr flush dev "$BRIDGE" scope global
  ip link set "$BRIDGE" down
  ip link delete "$BRIDGE" type bridge
  sysctl -w "net.ipv4.ip_forward=$old_forward" >/dev/null
  rm -f -- "$state"
  NETWORK_READY=0
  flock -u 8
}

RUN_DIR="$(runner_run_dir "$PROFILE" "$RUN_ID")"
LOG_DIR="$(runner_log_dir "$PROFILE")"
LOG_FILE="$(runner_run_file "$PROFILE" "$RUN_ID" log)"
STATUS_FILE="$(runner_run_file "$PROFILE" "$RUN_ID" status)"
PID_FILE="$(runner_run_file "$PROFILE" "$RUN_ID" pid)"

# Acquire VM ownership before this run writes any RUNNING/PID/log metadata.
# Distinct Linux VMIDs therefore have independent locks and can run together.
LOCK_FILE="$(runner_lock_file "$PROFILE" "$VM_ID")"
exec 9>"$LOCK_FILE"
if ! flock -n 9; then
  mkdir -p "$LOG_DIR"
  printf '=== Starting disposable %s CI run %s ===\n' "$PROFILE" "$RUN_ID" > "$LOG_FILE"
  printf 'FAILED\n' > "$STATUS_FILE"
  printf 'Another %s CI runner already owns VM %s (%s).\n' "$PROFILE" "$VM_ID" "$LOCK_FILE" >> "$LOG_FILE"
  echo "Another $PROFILE CI runner already owns VM $VM_ID ($LOCK_FILE)." >&2
  rm -f -- "$PID_FILE"
  exit 75
fi

mkdir -p "$LOG_DIR" "$RUN_DIR"
echo "=== Starting disposable $PROFILE CI run $RUN_ID ===" > "$LOG_FILE"
exec >> "$LOG_FILE" 2>&1
echo "$$" > "$PID_FILE"
echo "RUNNING" > "$STATUS_FILE"

cleanup() {
  local exit_code=$?
  echo "$(ts) Cleaning up $PROFILE CI run $RUN_ID (exit code $exit_code)..."
  if [[ "$KEEP" == true && "$VM_CREATED" == 1 ]]; then
    echo "$(ts) Preserving VM $VM_ID and network $BRIDGE (--keep=true)"
  else
    config="$(qm config "$VM_ID" 2>/dev/null || true)"
    vm_name="$(awk -F': ' '$1 == "name" {print substr($0, index($0, ": ") + 2); exit}' <<<"$config")"
    vm_description="$(awk -F': ' '$1 == "description" {print substr($0, index($0, ": ") + 2); exit}' <<<"$config")"
    if [[ -n "$config" && "$VM_CREATED" == 1 && "$vm_name" == "ra8-lab-$PROFILE-$RUN_ID" &&
          ( -z "$vm_description" || "$vm_description" == *"RA8_LAB_RUN=$RUN_ID"* ) ]]; then
      echo "$(ts) Stopping and destroying run-owned VM $VM_ID..."
      qm stop "$VM_ID" --timeout 15 >/dev/null 2>&1 || true
      for _ in {1..30}; do
        [[ "$(qm status "$VM_ID" 2>/dev/null | awk '{print $2}')" == "stopped" ]] && break
        sleep 1
      done
      qm set "$VM_ID" --protection 0 >/dev/null 2>&1 || true
      qm destroy "$VM_ID" --purge 1 >/dev/null 2>&1 || true
    elif [[ -n "$config" ]]; then
      echo "$(ts) Refusing to alter VM $VM_ID because it is not owned by run $RUN_ID."
    fi
    if [[ "$PROFILE" == linux ]]; then
      cleanup_shared_linux_network || exit_code=1
    else
      if ip link show "$BRIDGE" >/dev/null 2>&1; then
        echo "$(ts) Tearing down $BRIDGE and firewall..."
        ip addr flush dev "$BRIDGE" scope global >/dev/null 2>&1 || true
        ip link delete "$BRIDGE" type bridge >/dev/null 2>&1 || true
      fi
      nft delete table ip "$NFT_TABLE" >/dev/null 2>&1 || true
    fi
  fi
  # The Windows inventory carries the credential at the point of use, so it does
  # not outlive the run that needed it.
  rm -f "$RUN_DIR/windows-inventory.ini"
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
if qm status "$VM_ID" >/dev/null 2>&1; then
  echo "$(ts) error: reserved VMID $VM_ID is already occupied; refusing to reuse it."
  exit 1
fi
if [[ "$PROFILE" == linux ]]; then
  setup_shared_linux_network
else
  UPLINK_ROUTE="$(ip -o route get 1.1.1.1)" || { echo "$(ts) error: no Proxmox uplink route."; exit 1; }
  UPLINK="$(derive_uplink_from_route "$UPLINK_ROUTE")" || { echo "$(ts) error: unsafe uplink route: $UPLINK_ROUTE"; exit 1; }
  [[ ! -e /sys/class/net/"$BRIDGE" ]] || { echo "$(ts) error: $BRIDGE already exists."; exit 1; }
  ip link add "$BRIDGE" type bridge
  ip addr add "$GATEWAY/24" dev "$BRIDGE"
  ip link set "$BRIDGE" up
  sysctl -w net.ipv4.ip_forward=1 >/dev/null
  nft -f - <<EOF
table ip $NFT_TABLE {
  set lab_ingress { type ifname; elements = { "$BRIDGE", "fwbr${VM_ID}i0", "fwln${VM_ID}i0", "fwpr${VM_ID}p0" } }
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
    iifname @lab_ingress oifname "$UPLINK" udp dport 53 accept
    iifname @lab_ingress oifname "$UPLINK" tcp dport 53 accept
    iifname @lab_ingress oifname "$UPLINK" tcp dport { 80, 443 } accept
    iifname @lab_ingress drop
  }
  chain postrouting {
    type nat hook postrouting priority srcnat; policy accept;
    oifname "$UPLINK" ip saddr $SUBNET masquerade
  }
}
EOF
fi

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
VM_CREATED=1
qm set "$VM_ID" --description "Disposable RA8 lab VM; RA8_LAB_RUN=$RUN_ID"
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

  # Point of use. The credential is copied byte for byte out of the root-owned
  # file into the inventory the playbook reads, and never becomes a shell
  # variable, an exported name, or a command-line argument on the way, so it
  # cannot surface in a process listing or a dump of this script. The inventory
  # is created 0600 before a byte is written and removed by cleanup().
  INVENTORY="$RUN_DIR/windows-inventory.ini"
  if ! windows_credential_present "$WINDOWS_CREDENTIAL_FILE"; then
    echo "$(ts) error: Windows lab credential is no longer present at $WINDOWS_CREDENTIAL_FILE."
    exit 1
  fi
  install -m 600 /dev/null "$INVENTORY"
  {
    printf '[lab_windows]\n'
    printf 'ra8-lab-windows ansible_host=%s ansible_port=5985 ansible_user=%s ansible_connection=winrm ansible_winrm_transport=ntlm ansible_winrm_server_cert_validation=ignore ansible_password=' \
      "$GUEST_IP" "$GUEST_USER"
    tr -d '\r\n' < "$WINDOWS_CREDENTIAL_FILE"
    printf '\n\n[lab_windows:vars]\nansible_host_key_checking=False\n'
  } > "$INVENTORY"

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
