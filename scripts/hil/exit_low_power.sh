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
# This internal suite helper inherits the suite's repository bench lock. An
# off-bench caller stages the recovery image and four validated rig values,
# then re-enters this exact operation on the declared bench host.

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

  _hil_entry="${BASH_SOURCE[0]:-}"
  if [[ -n "$_hil_entry" ]]; then
    _hil_dir="$(cd "$(dirname "$_hil_entry")" && pwd -P)"
    _hil_entry="$_hil_dir/$(basename "$_hil_entry")"
  else
    _hil_dir="$(pwd -P)"
  fi
  # shellcheck source=scripts/hil/lib/rig_env.sh
  source "$_hil_dir/lib/rig_env.sh"
  rig_require JLINK_SN JLINK_DEVICE
  # shellcheck source=scripts/hil/lib/privileged_helper.sh
  source "$_hil_dir/lib/privileged_helper.sh"

  APP="${1:-low-power firmware}"
  RECOVERY_HEX="${2:-}"
  MANIFEST_OVERRIDE="${3:-}"
  MAX_ATTEMPTS="${RA8_LOW_POWER_HALT_ATTEMPTS:-30}"
  if [[ ! "$MAX_ATTEMPTS" =~ ^[0-9]+$ ]] || ((MAX_ATTEMPTS < 1 || MAX_ATTEMPTS > 100)); then
    echo "exit_low_power: RA8_LOW_POWER_HALT_ATTEMPTS must be 1..100" >&2
    exit 2
  fi

  [[ -n "$RECOVERY_HEX" && -f "$RECOVERY_HEX" ]] || {
    echo "exit_low_power: a built follow-on HEX is required" >&2
    exit 2
  }

  if [[ -n "$MANIFEST_OVERRIDE" ]]; then
    [[ "$MANIFEST_OVERRIDE" =~ ^/tmp/ra8_low_power\.[A-Za-z0-9]{8}/helper[.]sha256$ &&
      -f "$MANIFEST_OVERRIDE" && ! -L "$MANIFEST_OVERRIDE" ]] || {
      echo "exit_low_power: candidate helper manifest path is unsafe" >&2
      exit 2
    }
    manifest_identity="$(stat -c '%u:%a' -- "$MANIFEST_OVERRIDE")"
    [[ "$manifest_identity" == "$(id -u):600" ]] || {
      echo "exit_low_power: candidate helper manifest identity is unsafe" >&2
      exit 2
    }
    _ra8_hil_privileged_manifest="$MANIFEST_OVERRIDE"
  fi

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
  trap 'rm -f "$stripped_hex"' EXIT
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

  if ! rig_is_local_pi; then
    rig_require PI_HOST PI_REPO
    remote_dir="$(ssh "$PI_HOST" 'umask 077; mktemp -d /tmp/ra8_low_power.XXXXXXXX')" || {
      echo "exit_low_power: could not allocate private remote staging" >&2
      exit 2
    }
    [[ "$remote_dir" =~ ^/tmp/ra8_low_power\.[A-Za-z0-9]{8}$ ]] || {
      echo "exit_low_power: remote staging directory has an invalid identity" >&2
      exit 2
    }
    remote_hex="${remote_dir}/recovery.hex"
    remote_rig="${remote_dir}/rig.env"
    remote_manifest="${remote_dir}/helper.sha256"
    printf -v remote_dir_q '%q' "$remote_dir"
    printf -v remote_hex_q '%q' "$remote_hex"
    printf -v remote_rig_q '%q' "$remote_rig"
    printf -v remote_manifest_q '%q' "$remote_manifest"
    printf -v remote_repo_q '%q' "${PI_REPO}/scripts/hil"
    printf -v remote_app_q '%q' "$APP"
    printf -v remote_lock_q '%q' "${RA8_BENCH_LOCK_ID:-}"
    printf -v remote_pi_host_q '%q' "$PI_HOST"
    printf -v remote_jlink_sn_q '%q' "$JLINK_SN"
    printf -v remote_jlink_device_q '%q' "$JLINK_DEVICE"
    printf -v remote_pi_repo_q '%q' "$PI_REPO"
    # shellcheck disable=SC2329  # registered with the shared EXIT dispatcher.
    cleanup_remote_low_power() {
      # shellcheck disable=SC2029  # The validated remote mktemp path is intentionally expanded client-side.
      ssh "$PI_HOST" "rm -rf -- ${remote_dir_q}" >/dev/null 2>&1 || true
    }
    _ra8_bench_add_exit_trap cleanup_remote_low_power
    scp -q "$stripped_hex" "${PI_HOST}:${remote_hex}"
    scp -q "$_hil_dir/lib/ra8-hil-privileged.sha256" "${PI_HOST}:${remote_manifest}"
    # shellcheck disable=SC2029  # The remotely allocated path is quoted above.
    ssh "$PI_HOST" "chmod 0600 -- ${remote_manifest_q}"
    # The validated grammar makes each %q value byte-identical. Stream them so
    # protected rig coordinates never enter ssh argv.
    # shellcheck disable=SC2029  # The validated remote rig path is intentionally expanded client-side.
    if ! printf 'PI_HOST=%s\nJLINK_SN=%s\nJLINK_DEVICE=%s\nPI_REPO=%s\n' \
      "$remote_pi_host_q" "$remote_jlink_sn_q" "$remote_jlink_device_q" \
      "$remote_pi_repo_q" |
      ssh "$PI_HOST" "umask 077; /bin/cat >${remote_rig_q}"; then
      echo "exit_low_power: could not stage protected remote rig values" >&2
      exit 2
    fi
    # shellcheck disable=SC2029  # Every remote value is quoted immediately above.
    ssh "$PI_HOST" "cd ${remote_repo_q} || exit 2; \
      RA8_RIG_ENV=${remote_rig_q} RA8_BENCH_LOCK_ID=${remote_lock_q} \
      /bin/bash -p -s -- ${remote_app_q} ${remote_hex_q} ${remote_manifest_q}" \
      <"$_hil_entry"
    exit $?
  fi
  ra8_hil_privileged_verify_local || exit $?

  log="/tmp/hil_exit_low_power_${APP//[^A-Za-z0-9_.-]/_}.$$.log"
  tmp_script="$(mktemp)"
  # shellcheck disable=SC2329  # registered with the shared EXIT dispatcher.
  cleanup_low_power_script() { rm -f "$tmp_script"; }
  _ra8_bench_add_exit_trap cleanup_low_power_script
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
  # Port 3 is the fixed J-Link OB control path; target USB tests use the other
  # declared downstream ports and must never cycle this one.
  if [[ "${matches[0]}" != "2-1.3.3" ]]; then
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
