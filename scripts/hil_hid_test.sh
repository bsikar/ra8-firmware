#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# hil_hid_test.sh -- HIL probe for a USB-HID device app on the EK-RA8D2.
# Flashes the app, then confirms on the USB host (the bench Pi) that the
# device enumerates and the kernel binds it as a HID device (hidraw +
# input node). A USB-HID app cannot use HIL_MODE=jlink_memprobe: halting
# the core to read a counter stalls the SIE and breaks enumeration, so
# the device-class state must be observed host-side instead.
#
# Usage: bash scripts/hil_hid_test.sh --app <app-name> [--vidpid VVVV:PPPP]
# Exit:  0 = kernel bound the device as USB HID
#        1 = failure (no flash / no HID binding)
#        2 = usage error

set -euo pipefail

# Rig config (PI_HOST) comes from the gitignored .env, not the tree.
_hil_dir="$(dirname "${BASH_SOURCE[0]}")"
_hil_dir="$(cd "$_hil_dir" && pwd)"
# shellcheck source=scripts/lib/rig_env.sh
source "$_hil_dir/lib/rig_env.sh"
rig_require PI_HOST
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

APP=""
VIDPID="1209:0001"
ENUM_WAIT_S=14
while [[ $# -gt 0 ]]; do
  case "$1" in
    --app)
      APP="$2"
      shift 2
      ;;
    --vidpid)
      VIDPID="$2"
      shift 2
      ;;
    --wait)
      ENUM_WAIT_S="$2"
      shift 2
      ;;
    -h | --help)
      sed -n '5,14p' "$0"
      exit 0
      ;;
    *)
      echo "Unknown arg: $1"
      exit 2
      ;;
  esac
done
[[ -n "$APP" ]] || {
  echo "Usage: $0 --app <app-name> [--vidpid VVVV:PPPP]"
  exit 2
}

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

# Run a command on the Pi (locally if we ARE the Pi, else over SSH).
LOCAL_PI=0
if [[ "$(hostname 2>/dev/null || true)" =~ ^star ]]; then LOCAL_PI=1; fi
# shellcheck disable=SC2029  # the caller composes the remote command; forwarding it verbatim is the point.
pi_run() { if ((LOCAL_PI)); then bash -c "$*"; else ssh "$PI_HOST" "$*"; fi; }

echo -e "${YELLOW}[HID]${NC} app=${APP}  vidpid=${VIDPID}"

# Clear the host kernel log, flash, let the device enumerate + bind.
pi_run "sudo -n dmesg -C" >/dev/null 2>&1 || true
if ! bash "${SCRIPT_DIR}/hil_flash.sh" "${APP}" >/dev/null 2>&1; then
  echo -e "${RED}[HID FAIL]${NC} ${APP}: flash failed"
  exit 1
fi
echo -e "${YELLOW}[HID]${NC} flashed; waiting ${ENUM_WAIT_S}s for enumeration..."
sleep "${ENUM_WAIT_S}"

VID="${VIDPID%%:*}"
DM="$(pi_run "sudo -n dmesg -T 2>/dev/null" || true)"
echo "--- host kernel log (HID) ---"
echo "$DM" | grep -iE "New USB device found.*${VID}|hidraw|input:.*HID|usbhid|unable to enumerate" |
  tail -8 || true
echo "--- end ---"

# Pass iff the kernel's hid-generic driver bound the interface -- the
# "input,hidrawN" / "hidrawN ... USB HID" line only prints once the HID
# Report Descriptor handshake over EP0 has completed.
if echo "$DM" | grep -qiE "input,hidraw[0-9]|hidraw[0-9].*USB HID"; then
  echo -e "${GREEN}[HID PASS]${NC} ${APP}: kernel bound the device as USB HID"
  exit 0
fi

# Issue #58: usbhid probe occasionally fails with -32 (EPIPE) when the
# USB-FS bus state from a previous test leaks into this enumeration.
# Force a clean re-enum via soft PPPS (authorized toggle on the USBFS
# hub port 4 -- hard PPPS doesn't bus-reset USBFS reliably; see
# scripts/hil_ppps.sh header) and check once more before failing.
echo -e "${YELLOW}[HID]${NC} no hidraw on first attempt; soft-PPPS retry..."
pi_run "sudo -n dmesg -C" >/dev/null 2>&1 || true
bash "${SCRIPT_DIR}/hil_ppps.sh" --soft cycle 4 >/dev/null 2>&1 || true
sleep "${ENUM_WAIT_S}"
DM="$(pi_run "sudo -n dmesg -T 2>/dev/null" || true)"
echo "--- host kernel log (HID retry) ---"
echo "$DM" | grep -iE "New USB device found.*${VID}|hidraw|input:.*HID|usbhid|unable to enumerate" |
  tail -8 || true
echo "--- end ---"
if echo "$DM" | grep -qiE "input,hidraw[0-9]|hidraw[0-9].*USB HID"; then
  echo -e "${GREEN}[HID PASS]${NC} ${APP}: kernel bound the device as USB HID (after PPPS retry)"
  exit 0
fi

echo -e "${RED}[HID FAIL]${NC} ${APP}: no HID (hidraw) binding observed after retry"
exit 1
