#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# hil_ppps.sh -- Force a USB re-enumeration of a hub-port-attached device
# without cutting board power. Two mechanisms are supported:
#
#   1. Hardware PPPS via `uhubctl` (default): the VIA Labs hub at 2-1.3
#      toggles data/power per-port at the USB level. Works reliably for
#      USBHS J7 (port 1) and the J-Link OB (port 2). On USBFS J11
#      (port 4) the device's D+ pull-up stays asserted across the hub-
#      side toggle and the host's xhci-hcd does NOT issue a fresh bus
#      reset, so re-enumeration silently fails.
#
#   2. Host-side `authorized` toggle (--soft): write 0 then 1 to
#      `/sys/bus/usb/devices/<bus-port>/authorized`. The kernel
#      treats the unauthorized->authorized transition as a fresh
#      attach and re-runs the chapter-9 enumeration. This is the
#      reliable path for USBFS where hardware PPPS does not work.
#
# EK-RA8D2 wiring on hub 2-1.3:
#   port 1 -> J7  USBHS  (1209:000c when HS firmware is running)
#   port 2 -> J-Link OB  (1366:1024 always)
#   port 4 -> J11 USBFS  (1209:000a when FS firmware is running)
#
# Usage (run from repo root on dev machine, or directly on the Pi):
#   bash scripts/hil_ppps.sh [--soft] <off|on|cycle> [port]
#     --soft : use host-side `authorized` toggle (no uhubctl).
#     port   : defaults to 2 (J-Link). Pick 1 for USBHS, 4 for USBFS.
#
# Exit codes:
#   0 -- command succeeded
#   1 -- uhubctl error
#   2 -- usage error or Pi unreachable

set -euo pipefail

# Rig config (PI_HOST) comes from the gitignored .env, not the tree.
_hil_dir="$(dirname "${BASH_SOURCE[0]}")"
_hil_dir="$(cd "$_hil_dir" && pwd)"
# shellcheck source=scripts/lib/rig_env.sh
source "$_hil_dir/lib/rig_env.sh"
rig_require PI_HOST
HUB="2-1.3"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

SOFT=0
ARGS=()
for arg in "$@"; do
  case "$arg" in
    --soft) SOFT=1 ;;
    *) ARGS+=("$arg") ;;
  esac
done
set -- "${ARGS[@]}"

[[ $# -ge 1 && $# -le 2 ]] || {
  echo "Usage: $0 [--soft] <off|on|cycle> [port]"
  exit 2
}
CMD="$1"
PORT="${2:-2}"

RUN_LOCAL=0
if rig_is_local_pi; then
  RUN_LOCAL=1
fi

if ((RUN_LOCAL == 0)); then
  ssh -o ConnectTimeout=5 -o BatchMode=yes "$PI_HOST" true 2>/dev/null ||
    {
      echo -e "${RED}[ERROR]${NC} cannot reach ${PI_HOST}"
      exit 2
    }
fi

pi_sh() {
  if ((RUN_LOCAL)); then
    bash -c "$*"
  else
    # shellcheck disable=SC2029  # the caller composes the remote command; forwarding it verbatim is the point.
    ssh "$PI_HOST" "$*"
  fi
}

auth_path="/sys/bus/usb/devices/${HUB}.${PORT}/authorized"

auth_write() {
  pi_sh "echo $1 | sudo -n tee ${auth_path} >/dev/null 2>&1"
}

hard_off() { pi_sh "sudo -n uhubctl -l ${HUB} -p ${PORT} -a off" 2>&1; }
hard_on() { pi_sh "sudo -n uhubctl -l ${HUB} -p ${PORT} -a on" 2>&1; }

case "$CMD" in
  off)
    if ((SOFT)); then
      echo -e "${YELLOW}[hil_ppps]${NC} port ${PORT} unauthorize"
      auth_write 0
    else
      echo -e "${YELLOW}[hil_ppps]${NC} port ${PORT} OFF (uhubctl)"
      hard_off
    fi
    ;;
  on)
    if ((SOFT)); then
      echo -e "${YELLOW}[hil_ppps]${NC} port ${PORT} authorize"
      auth_write 1
    else
      echo -e "${YELLOW}[hil_ppps]${NC} port ${PORT} ON (uhubctl)"
      hard_on
    fi
    ;;
  cycle)
    if ((SOFT)); then
      echo -e "${YELLOW}[hil_ppps]${NC} port ${PORT} authorized-cycle"
      auth_write 0
      sleep 1
      auth_write 1
    else
      echo -e "${YELLOW}[hil_ppps]${NC} port ${PORT} OFF (uhubctl)"
      hard_off
      sleep 1
      echo -e "${YELLOW}[hil_ppps]${NC} port ${PORT} ON (uhubctl)"
      hard_on
    fi
    ;;
  *)
    echo "Usage: $0 [--soft] <off|on|cycle> [port]"
    exit 2
    ;;
esac

echo -e "${GREEN}[hil_ppps DONE]${NC} ${CMD} port ${PORT}$( ((SOFT)) && echo " (soft)")"
