#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# Restore a haltable EK-RA8D2 after firmware deliberately enters a low-power
# mode that gates the AHB-AP. A plain rfp-cli Initialize cannot reach that
# state: it returns E100000E because the protection/debug path is already
# asleep. The reliable boundary is a true target power cycle followed by a
# fast J-Link halt before the same firmware can re-enter low power. Merely
# quitting after the halt resumes or resets the old image on this probe, so the
# helper installs a known-safe follow-on image in that same debugger session.
#
# This is an internal suite helper and runs on the bench host, where all.sh has
# staged the complete scripts/hil tree. It inherits the suite's repository
# bench lock before touching target power.

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

  _hil_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  # shellcheck source=scripts/hil/lib/rig_env.sh
  source "$_hil_dir/lib/rig_env.sh"
  rig_require JLINK_SN
  # shellcheck source=scripts/hil/lib/privileged_helper.sh
  source "$_hil_dir/lib/privileged_helper.sh"

  APP="${1:-low-power firmware}"
  RECOVERY_HEX="${2:-}"
  MAX_ATTEMPTS="${RA8_LOW_POWER_HALT_ATTEMPTS:-30}"
  if [[ ! "$MAX_ATTEMPTS" =~ ^[0-9]+$ ]] || ((MAX_ATTEMPTS < 1 || MAX_ATTEMPTS > 100)); then
    echo "exit_low_power: RA8_LOW_POWER_HALT_ATTEMPTS must be 1..100" >&2
    exit 2
  fi

  [[ -n "$RECOVERY_HEX" && -f "$RECOVERY_HEX" ]] || {
    echo "exit_low_power: a built follow-on HEX is required" >&2
    exit 2
  }

  # Validate the caller's full follow-on image before removing option sections.
  # Checking only the stripped copy would hide the exact lockdown bytes this gate
  # exists to refuse.
  # shellcheck source=scripts/hil/lib/preflash_guard.sh
  source "$_hil_dir/lib/preflash_guard.sh"
  ra8_preflash_guard "$RECOVERY_HEX" || exit $?

  # Program only MRAM. The normal application image also contains option/OTP
  # sections, which are intentionally excluded from this bounded recovery path.
  recovery_elf="${RECOVERY_HEX%.hex}.elf"
  stripped_hex="$(mktemp "/tmp/hil_low_power_recovery.XXXXXX.hex")"
  if [[ -f "$recovery_elf" ]]; then
    arm-none-eabi-objcopy --remove-section='.option_setting*' -O ihex \
      "$recovery_elf" "$stripped_hex" 2>/dev/null || cp "$RECOVERY_HEX" "$stripped_hex"
  else
    arm-none-eabi-objcopy -I ihex --remove-section='.option_setting*' -O ihex \
      "$RECOVERY_HEX" "$stripped_hex" 2>/dev/null || cp "$RECOVERY_HEX" "$stripped_hex"
  fi

  # shellcheck source=scripts/hil/lib/bench_lock.sh
  source "$_hil_dir/lib/bench_lock.sh"
  ra8_bench_require_recovery "restore debug after ${APP}" 5m || exit $?

  rig_is_local_pi || {
    echo "exit_low_power: internal helper must run on the declared bench host" >&2
    exit 2
  }
  ra8_hil_privileged_verify_local || exit $?

  log="/tmp/hil_exit_low_power_${APP//[^A-Za-z0-9_.-]/_}.$$.log"
  tmp_script="$(mktemp)"
  trap 'rm -f "$tmp_script" "$stripped_hex"' EXIT
  cat >"$tmp_script" <<JLINK
device ${JLINK_DEVICE}
si SWD
speed 4000
connect
halt
loadfile ${stripped_hex}
r
g
q
JLINK

  declare -a matches=()
  declared_serial="$(printf '%s' "$JLINK_SN" | sed 's/^0*//')"
  [[ -n "$declared_serial" ]] || declared_serial=0
  for serial_file in /sys/bus/usb/devices/*/serial; do
    [[ -r "$serial_file" ]] || continue
    found_serial="$(printf '%s' "$(<"$serial_file")" | sed 's/^0*//')"
    [[ -n "$found_serial" ]] || found_serial=0
    [[ "$found_serial" == "$declared_serial" ]] || continue
    matches+=("$(basename "$(dirname "$serial_file")")")
  done
  if ((${#matches[@]} != 1)); then
    echo "exit_low_power: expected one USB J-Link for ${JLINK_SN}; found ${#matches[@]}" >&2
    exit 1
  fi
  if [[ "${matches[0]}" != "2-1.3.2" ]]; then
    echo "exit_low_power: J-Link is outside the declared bench USB topology" >&2
    exit 1
  fi

  echo "[exit-low-power] ${APP}: cycling declared root USB hub 2-1"
  ra8_hil_privileged_run_local usb-root-cycle >/dev/null 2>&1

  for ((attempt = 1; attempt <= MAX_ATTEMPTS; attempt++)); do
    : >"$log"
    timeout 8s JLinkExe -nogui 1 -SelectEmuBySN "$JLINK_SN" \
      -commanderscript "$tmp_script" >"$log" 2>&1 || true
    if grep -q "Cortex-M85 identified" "$log" && grep -Eq "Downloading file|Contents already match" "$log" &&
      grep -q "O\.K\." "$log" &&
      ! grep -qiE "CPU could not be halted|Cannot connect to the probe|Failed to configure AP|RAMCode did not respond|Unspecified error|Writing target memory failed|Failed to prepare" "$log"; then
      echo "[exit-low-power] ${APP}: follow-on image installed after POR (attempt ${attempt}/${MAX_ATTEMPTS})"
      echo "[exit-low-power] evidence: ${log}"
      exit 0
    fi
    sleep 0.1
  done

  echo "exit_low_power: could not install the follow-on image after ${MAX_ATTEMPTS} bounded attempts" >&2
  tail -25 "$log" >&2
  echo "exit_low_power: refusing to run another app against an unknown board state" >&2
  exit 1
else
  [[ "$-" == *p* ]]
fi
