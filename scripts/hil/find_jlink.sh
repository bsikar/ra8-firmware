#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# find_jlink.sh -- enumerate SEGGER J-Link serial numbers on the configured
# bench host, or on this host when it is the rig. The command only asks the
# SEGGER tools/USB inventory for attached probes; it never connects to, halts,
# erases, or programs a target.
#
# Detection order on the selected host:
#   1. JLinkExe ShowEmuList (the serial JLinkExe/rfp-cli expect).
#   2. Native USB enumeration: system_profiler on macOS, /sys on Linux.
#
# Usage:
#   /bin/bash -p scripts/hil/find_jlink.sh
#   /bin/bash -p scripts/hil/find_jlink.sh --selftest
#   just hil::find_jlink

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

  set -uo pipefail

  CYAN='\033[0;36m'
  GREEN='\033[0;32m'
  YELLOW='\033[1;33m'
  NC='\033[0m'
  tag() { printf "${CYAN}[find-jlink]${NC} %s\n" "$*"; }

  emit() {
    [ -n "${1:-}" ] || return 0
    printf "${GREEN}JLINK_SN=%s${NC}\n" "$1"
    FOUND=1
  }

  # Print serial numbers only, one per line. Keeping routing and presentation out
  # of this function lets its exact implementation run on the far side of SSH.
  find_jlink_scan_local() {
    local out d
    if command -v JLinkExe >/dev/null 2>&1; then
      out="$(printf 'ShowEmuList\nexit\n' | JLinkExe -nogui 1 2>/dev/null || true)"
      printf '%s\n' "$out" | grep -oE 'Serial number: [0-9]+' | grep -oE '[0-9]+'
      return 0
    fi

    case "$(uname -s)" in
      Darwin)
        system_profiler SPUSBDataType 2>/dev/null |
          awk '/J-Link/{f=1} f&&/Serial Number:/{print $NF; f=0}'
        ;;
      Linux)
        for d in /sys/bus/usb/devices/*; do
          [ -r "$d/idVendor" ] || continue
          [ "$(cat "$d/idVendor" 2>/dev/null)" = "1366" ] || continue
          [ -r "$d/serial" ] && cat "$d/serial"
        done
        ;;
    esac
  }

  find_jlink_scan_remote() {
    {
      declare -f find_jlink_scan_local
      printf '%s\n' find_jlink_scan_local
    } | ssh -o ConnectTimeout=5 -o BatchMode=yes "$PI_HOST" /bin/bash -p -s
  }

  find_jlink_run() {
    local serials sn
    FOUND=0
    if [ -n "${PI_HOST:-}" ] && ! rig_is_local_pi; then
      tag "querying the configured bench host over SSH..."
      if ! serials="$(find_jlink_scan_remote)"; then
        printf '%sCould not query J-Link inventory on the configured bench.%s\n' \
          "$YELLOW" "$NC" >&2
        return 2
      fi
    else
      tag "querying J-Link inventory on this host..."
      serials="$(find_jlink_scan_local)"
    fi

    while IFS= read -r sn; do
      emit "$sn"
    done < <(printf '%s\n' "$serials" | awk 'NF && !seen[$0]++')

    if [ "$FOUND" -eq 0 ]; then
      printf '%sNo J-Link probe found.%s Check USB and board power, then retry.\n' \
        "$YELLOW" "$NC" >&2
      return 1
    fi
    return 0
  }

  find_jlink_assert_contains() {
    local text="$1" expected="$2" label="$3"
    case "$text" in
      *"$expected"*) return 0 ;;
      *)
        printf 'selftest: must-fire failed: %s\n' "$label" >&2
        return 1
        ;;
    esac
  }

  find_jlink_assert_absent() {
    local text="$1" refused="$2" label="$3"
    case "$text" in
      *"$refused"*)
        printf 'selftest: must-stay-quiet failed: %s\n' "$label" >&2
        return 1
        ;;
      *) return 0 ;;
    esac
  }

  find_jlink_selftest_remote() {
    local tmp output rc PI_HOST='remote-host-secret' JLINK_SN='configured-serial-secret'
    tmp="$1"
    rig_is_local_pi() { return 1; }
    ssh() {
      cat >/dev/null
      printf 'remote\n' >>"$tmp/routes"
      printf '111111111\n'
    }
    JLinkExe() {
      printf 'local\n' >>"$tmp/routes"
      return 1
    }

    if output="$(find_jlink_run 2>&1)"; then rc=0; else rc=$?; fi
    [ "$rc" -eq 0 ] || return 1
    find_jlink_assert_contains "$(cat "$tmp/routes")" remote "remote route did not invoke SSH"
    find_jlink_assert_absent "$(cat "$tmp/routes")" local "remote route invoked local scan"
    find_jlink_assert_contains "$output" 'JLINK_SN=111111111' "remote serial was not emitted"
    find_jlink_assert_absent "$output" "$PI_HOST" "remote host leaked to output"
    find_jlink_assert_absent "$output" "$JLINK_SN" "configured serial leaked to output"
  }

  find_jlink_selftest_local() {
    local tmp output rc PI_HOST='remote-host-secret' JLINK_SN='configured-serial-secret'
    tmp="$1"
    rig_is_local_pi() { return 0; }
    ssh() {
      cat >/dev/null
      printf 'remote\n' >>"$tmp/routes"
      return 1
    }
    JLinkExe() {
      cat >/dev/null
      printf 'local\n' >>"$tmp/routes"
      printf 'J-Link[0]: Connection: USB, Serial number: 222222222, Product: J-Link OB\n'
    }

    if output="$(find_jlink_run 2>&1)"; then rc=0; else rc=$?; fi
    [ "$rc" -eq 0 ] || return 1
    find_jlink_assert_contains "$(cat "$tmp/routes")" local "local route did not scan locally"
    find_jlink_assert_absent "$(cat "$tmp/routes")" remote "local route invoked SSH"
    find_jlink_assert_contains "$output" 'JLINK_SN=222222222' "local serial was not emitted"
    find_jlink_assert_absent "$output" "$PI_HOST" "local route leaked configured host"
    find_jlink_assert_absent "$output" "$JLINK_SN" "local route leaked configured serial"
  }

  find_jlink_selftest_failure() {
    local tmp output rc PI_HOST='remote-host-secret' JLINK_SN='configured-serial-secret'
    tmp="$1"
    rig_is_local_pi() { return 1; }
    ssh() {
      cat >/dev/null
      printf 'remote-failed\n' >>"$tmp/routes"
      return 255
    }

    if output="$(find_jlink_run 2>&1)"; then rc=0; else rc=$?; fi
    [ "$rc" -eq 2 ] || return 1
    find_jlink_assert_contains "$(cat "$tmp/routes")" remote-failed \
      "remote transport failure did not fire"
    find_jlink_assert_contains "$output" 'Could not query J-Link inventory' \
      "remote transport failure stayed quiet"
    find_jlink_assert_absent "$output" "$PI_HOST" "failed route leaked configured host"
    find_jlink_assert_absent "$output" "$JLINK_SN" "failed route leaked configured serial"
  }

  find_jlink_selftest() (
    set -euo pipefail
    local tmp
    tmp="$(mktemp -d "${TMPDIR:-/tmp}/ra8-find-jlink.XXXXXX")"
    trap 'rm -rf "$tmp"' EXIT
    : >"$tmp/routes"
    (find_jlink_selftest_remote "$tmp")
    : >"$tmp/routes"
    (find_jlink_selftest_local "$tmp")
    : >"$tmp/routes"
    (find_jlink_selftest_failure "$tmp")
    printf '%s\n' \
      'find_jlink.sh --selftest: PASS (6 must-fire, 8 must-stay-quiet assertions)'
  )

  case "${1:-}" in
    --selftest)
      [ "$#" -eq 1 ] || {
        echo 'usage: find_jlink.sh [--selftest]' >&2
        exit 2
      }
      find_jlink_selftest
      exit
      ;;
    '') ;;
    *)
      echo 'usage: find_jlink.sh [--selftest]' >&2
      exit 2
      ;;
  esac

  _hil_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  # shellcheck source=scripts/hil/lib/rig_env.sh
  source "$_hil_dir/lib/rig_env.sh"
  unset _hil_dir
  find_jlink_run
else
  [[ "$-" == *p* ]]
fi
