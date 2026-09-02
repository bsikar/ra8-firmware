#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# hil_hid_test.sh -- HIL probe for a USB-HID device app on the EK-RA8D2.
# Flashes the app, then confirms on the USB host (the bench Pi) that the
# device enumerates and the kernel binds it as a HID device (hidraw +
# input node). A USB-HID app cannot use HIL_MODE=jlink_memprobe: halting
# the core to read a counter stalls the SIE and breaks enumeration, so
# the device-class state must be observed host-side instead.
#
# Usage: /bin/bash -p scripts/hil/hid_test.sh --app <app-name> [--vidpid VVVV:PPPP]
# Exit:  0 = kernel bound the device as USB HID
#        1 = failure (no flash / no HID binding)
#        2 = usage error

if [[ "$-" == *p* ]]; then
  unset -v BASH_ENV ENV
  declare -a ra8_startup_env_unset=()
  _ra8_startup_refuse() {
    printf 'error: privileged startup %s\n' "$1" >&2
    exit 1
  }
  ra8_startup_env_done_count=0
  while IFS= read -r -d '' ra8_startup_env_row; do
    ra8_startup_env_name="${ra8_startup_env_row%%=*}"
    case "$ra8_startup_env_name" in
      RA8_STARTUP_ENV_DONE)
        ra8_startup_env_done_count=$((ra8_startup_env_done_count + 1))
        ;;
      BASH_FUNC_*%% | BASH_FUNC_*'()') ra8_startup_env_unset+=(-u "$ra8_startup_env_name") ;;
    esac
  done < <(
    /usr/bin/env -u RA8_STARTUP_ENV_DONE -0 &&
      /usr/bin/printf 'RA8_STARTUP_ENV_DONE=1\0'
  )
  ((ra8_startup_env_done_count == 1)) && [[ "$ra8_startup_env_name" == RA8_STARTUP_ENV_DONE ]] || _ra8_startup_refuse 'environment enumeration was incomplete'
  if ((${#ra8_startup_env_unset[@]})); then
    [[ -z "${RA8_STARTUP_ENV_SCRUBBED-}" ]] || _ra8_startup_refuse 'scrub did not converge'
    ra8_startup_reentry="$0"
    [[ "$ra8_startup_reentry" == */* ]] || _ra8_startup_refuse 'requires a script path'
    if [[ "$ra8_startup_reentry" != /* ]]; then
      ra8_startup_reentry="$PWD/$ra8_startup_reentry"
    fi
    ra8_startup_check="$ra8_startup_reentry"
    while [[ "$ra8_startup_check" != "/" ]]; do
      [[ ! -L "$ra8_startup_check" ]] || _ra8_startup_refuse 'refuses a symlinked path'
      ra8_startup_parent="${ra8_startup_check%/*}"
      [[ -n "$ra8_startup_parent" ]] || ra8_startup_parent="/"
      [[ "$ra8_startup_parent" != "$ra8_startup_check" ]] ||
        _ra8_startup_refuse 'cannot validate its script path'
      ra8_startup_check="$ra8_startup_parent"
    done
    [[ -f "$ra8_startup_reentry" ]] || _ra8_startup_refuse 'refuses a non-regular path'
    if ! exec /usr/bin/env "${ra8_startup_env_unset[@]}" -u BASH_ENV -u ENV \
      -u RA8_STARTUP_ENV_DONE RA8_STARTUP_ENV_SCRUBBED=1 \
      /bin/bash -p -- "$ra8_startup_reentry" "$@"; then
      _ra8_startup_refuse 'could not enter sanitized process'
    fi
  fi
  unset -v ra8_startup_check ra8_startup_env_done_count
  unset -v ra8_startup_env_name ra8_startup_env_row
  unset -v ra8_startup_env_unset ra8_startup_parent ra8_startup_reentry
  unset -v RA8_STARTUP_ENV_DONE
  unset -v RA8_STARTUP_ENV_SCRUBBED
  unset -f _ra8_startup_refuse

  set -euo pipefail

  # Rig config (PI_HOST) comes from the gitignored .env, not the tree.
  _hil_dir="$(dirname "${BASH_SOURCE[0]}")"
  _hil_dir="$(cd "$_hil_dir" && pwd)"
  # shellcheck source=scripts/hil/lib/rig_env.sh
  source "$_hil_dir/lib/rig_env.sh"
  rig_require PI_HOST

  # ---- bench mutual exclusion --------------------------------------------------
  # One actor at a time on the physical bench. The hold lives exactly as long as
  # this script does -- it is a live process on a kernel flock, not a lease -- so
  # nothing here can leave the bench stale. See scripts/hil/bench.sh.
  # shellcheck source=scripts/hil/lib/bench_lock.sh
  source "$_hil_dir/lib/bench_lock.sh"
  ra8_bench_require "hil usb-hid probe $*" || exit $?
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
  pi_run() { if ((LOCAL_PI)); then /bin/bash -p -c "$*"; else ssh "$PI_HOST" "$*"; fi; }

  echo -e "${YELLOW}[HID]${NC} app=${APP}  vidpid=${VIDPID}"

  # Clear the host kernel log, flash, let the device enumerate + bind.
  pi_run "sudo -n dmesg -C" >/dev/null 2>&1 || true
  if ! /bin/bash -p "${SCRIPT_DIR}/flash.sh" "${APP}" >/dev/null 2>&1; then
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
  # scripts/hil/ppps.sh header) and check once more before failing.
  echo -e "${YELLOW}[HID]${NC} no hidraw on first attempt; soft-PPPS retry..."
  pi_run "sudo -n dmesg -C" >/dev/null 2>&1 || true
  /bin/bash -p "${SCRIPT_DIR}/ppps.sh" --soft cycle 4 >/dev/null 2>&1 || true
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
else
  [[ "$-" == *p* ]]
fi
