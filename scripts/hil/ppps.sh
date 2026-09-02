#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
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
#   /bin/bash -p scripts/hil/ppps.sh [--soft] <off|on|cycle> [port]
#     --soft : use host-side `authorized` toggle (no uhubctl).
#     port   : defaults to 2 (J-Link). Pick 1 for USBHS, 4 for USBFS.
#
# Exit codes:
#   0 -- command succeeded
#   1 -- uhubctl error
#   2 -- usage error or Pi unreachable

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
  # shellcheck source=scripts/hil/lib/privileged_helper.sh
  source "$_hil_dir/lib/privileged_helper.sh"

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
  case "$CMD" in off | on | cycle) ;; *)
    echo "Usage: $0 [--soft] <off|on|cycle> [port]"
    exit 2
    ;;
  esac
  case "$PORT" in 1 | 2 | 4) ;; *)
    echo "hil_ppps: port must be one of 1, 2, or 4" >&2
    exit 2
    ;;
  esac

  # ---- bench mutual exclusion --------------------------------------------------
  # Hub ports 1/2/4 on 2-1.3 ARE J7, the J-Link and J11: cutting one mid-flash
  # takes the probe out from under whoever is using it.
  # shellcheck source=scripts/hil/lib/bench_lock.sh
  source "$_hil_dir/lib/bench_lock.sh"
  ra8_bench_require_recovery "hub port ${PORT}: ${CMD}" 10m || exit $?

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
    ra8_hil_privileged_verify_remote "$PI_HOST" || exit $?
  else
    ra8_hil_privileged_verify_local || exit $?
  fi

  pi_sh() {
    if ((RUN_LOCAL)); then
      /bin/bash -p -c "$*"
    else
      # shellcheck disable=SC2029  # the caller composes the remote command; forwarding it verbatim is the point.
      ssh "$PI_HOST" "$*"
    fi
  }

  auth_write() {
    pi_sh "sudo -n -- /usr/local/libexec/ra8-hil-privileged usb-authorize ${PORT} $1" \
      >/dev/null 2>&1
  }

  hard_off() {
    pi_sh "sudo -n -- /usr/local/libexec/ra8-hil-privileged usb-port-power ${PORT} off" 2>&1
  }
  hard_on() {
    pi_sh "sudo -n -- /usr/local/libexec/ra8-hil-privileged usb-port-power ${PORT} on" 2>&1
  }
  auth_cycle() {
    pi_sh "sudo -n -- /usr/local/libexec/ra8-hil-privileged usb-authorize-cycle ${PORT}" \
      >/dev/null 2>&1
  }
  hard_cycle() {
    pi_sh "sudo -n -- /usr/local/libexec/ra8-hil-privileged usb-port-cycle ${PORT}" 2>&1
  }

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
        auth_cycle
      else
        echo -e "${YELLOW}[hil_ppps]${NC} port ${PORT} transactional cycle (uhubctl)"
        hard_cycle
      fi
      ;;
  esac

  echo -e "${GREEN}[hil_ppps DONE]${NC} ${CMD} port ${PORT}$( ((SOFT)) && echo " (soft)")"
else
  [[ "$-" == *p* ]]
fi
