#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# hil_flash_retry.sh -- power-cycle an EK-RA8D2 and retry a bounded flash.
#
# A J-Link retry worker starts first and remains enumerated while the Tapo board
# plug performs a real POR. When target power returns, the worker immediately
# connects, halts, and programs MRAM before firmware can gate secure debug.
#
# Usage:
#   /bin/bash -p scripts/hil/flash_retry.sh <app>
#   /bin/bash -p scripts/hil/flash_retry.sh usb_cdc_echo

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

  # Rig config (PI_HOST, JLINK_SN) comes from the gitignored .env, not the tree.
  _hil_dir="$(dirname "${BASH_SOURCE[0]}")"
  _hil_dir="$(cd "$_hil_dir" && pwd)"
  # shellcheck source=scripts/hil/lib/rig_env.sh
  source "$_hil_dir/lib/rig_env.sh"
  rig_require PI_HOST JLINK_SN
  # shellcheck source=scripts/hil/lib/privileged_helper.sh
  source "$_hil_dir/lib/privileged_helper.sh"

  GREEN='\033[0;32m'
  RED='\033[0;31m'
  YELLOW='\033[1;33m'
  NC='\033[0m'

  [[ $# -eq 1 ]] || {
    echo "Usage: $0 <app>"
    exit 2
  }
  APP_ID="$1"

  SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

  APP_DIR="$(python3 "$ROOT/scripts/dev/ra8_apps.py" dir "$APP_ID" 2>/dev/null || true)"
  APP="$(python3 "$ROOT/scripts/dev/ra8_apps.py" name "$APP_ID" 2>/dev/null || true)"

  [[ -n "$APP_DIR" && -n "$APP" ]] || {
    echo -e "${RED}[ERROR]${NC} app '${APP_ID}' not found"
    exit 1
  }

  HEX="${APP_DIR}/build/${APP}.hex"
  if [[ ! -f "$HEX" ]]; then
    echo -e "${YELLOW}[hil_flash_retry]${NC} building ${APP_ID}..."
    /bin/bash -p "$ROOT/scripts/dev/run_just.sh" --justfile "$ROOT/justfile" apps::build "$APP_ID"
  fi
  [[ -f "$HEX" ]] || {
    echo -e "${RED}[ERROR]${NC} build failed"
    exit 1
  }

  # Program only MRAM. Full application HEX files also contain option/OTP
  # sections; retrying those after a POR produces "Writing target memory failed"
  # even when code MRAM was written, which made the old unbounded loop spin
  # forever after a successful recovery.
  ELF="${APP_DIR}/build/${APP}.elf"
  STRIPPED_HEX="$(mktemp "/tmp/hil_retry_${APP}_mram.XXXXXX.hex")"
  trap 'rm -f "$STRIPPED_HEX"' EXIT
  OFS_ARGS=('--remove-section=.option_setting*')
  if [[ -f "$ELF" ]]; then
    arm-none-eabi-objcopy "${OFS_ARGS[@]}" -O ihex "$ELF" "$STRIPPED_HEX" 2>/dev/null ||
      cp "$HEX" "$STRIPPED_HEX"
  else
    arm-none-eabi-objcopy -I ihex "${OFS_ARGS[@]}" -O ihex "$HEX" "$STRIPPED_HEX" 2>/dev/null ||
      cp "$HEX" "$STRIPPED_HEX"
  fi

  # ---- Anti-recovery pre-flash guard ------------------------------------------
  # Inspect the full image + source tree before any programming. See
  # scripts/hil/lib/preflash_guard.sh.
  # shellcheck source=scripts/hil/lib/preflash_guard.sh
  source "$_hil_dir/lib/preflash_guard.sh"
  ra8_preflash_guard "$HEX" || exit $?

  # ---- bench mutual exclusion --------------------------------------------------
  # Validate and build before taking the physical bench. The hold then spans the
  # upload, power cycle, and every J-Link retry as one indivisible operation.
  # shellcheck source=scripts/hil/lib/bench_lock.sh
  source "$_hil_dir/lib/bench_lock.sh"
  ra8_bench_require_recovery "power-cycle then flash ${APP_ID}" || exit $?
  ra8_hil_privileged_verify_remote "$PI_HOST" || exit $?

  echo -e "${YELLOW}[hil_flash_retry]${NC} uploading hex to Pi..."
  REMOTE_HEX="/tmp/hil_retry_${APP}_mram.$$.hex"
  scp -q "$STRIPPED_HEX" "${PI_HOST}:${REMOTE_HEX}"

  echo -e "${YELLOW}[hil_flash_retry]${NC} power-cycling the J-Link/target USB path..."

  # shellcheck disable=SC2087  # local vars (JLINK_SN, JLINK_DEVICE, etc.) expand client-side; remote vars are escaped
  ssh "$PI_HOST" /bin/bash -p <<REMOTE
set -uo pipefail

JLINK_SN="${JLINK_SN}"
JLINK_DEVICE="${JLINK_DEVICE}"
REMOTE_HEX="${REMOTE_HEX}"
APP="${APP}"
MAX_ATTEMPTS=30

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

cleanup_remote() {
    rm -f "\$REMOTE_HEX"
}
trap cleanup_remote EXIT INT TERM HUP

normalize_serial() {
  local value
  value=\$(printf '%s' "\$1" | sed 's/^0*//')
  printf '%s\n' "\${value:-0}"
}

declared_serial=\$(normalize_serial "\$JLINK_SN")
declare -a matches=()
for serial_file in /sys/bus/usb/devices/*/serial; do
  [[ -r "\$serial_file" ]] || continue
  [[ "\$(normalize_serial "\$(<"\$serial_file")")" == "\$declared_serial" ]] || continue
  matches+=("\$(basename "\$(dirname "\$serial_file")")")
done
if ((\${#matches[@]} != 1)); then
  printf "${RED}[hil_flash_retry FAIL]${NC} expected one USB J-Link; found %d\n" \
      "\${#matches[@]}" >&2
  exit 1
fi
if [[ "\${matches[0]}" != "2-1.3.2" ]]; then
  echo "${RED}[hil_flash_retry FAIL]${NC} J-Link is outside the declared bench USB topology" >&2
  exit 1
fi

sudo -n -- /usr/local/libexec/ra8-hil-privileged usb-root-cycle >/dev/null 2>&1

for ((attempt = 1; attempt <= MAX_ATTEMPTS; attempt++)); do
      LOG="/tmp/hil_retry_\${APP}.\$\$.log"

      TMP=\$(mktemp)
      cat > "\$TMP" <<JLINK
device \${JLINK_DEVICE}
si SWD
speed 4000
connect
halt
loadfile \${REMOTE_HEX}
r
g
q
JLINK

      timeout 8s JLinkExe -nogui 1 -SelectEmuBySN "\$JLINK_SN" -commanderscript "\$TMP" \
          > "\$LOG" 2>&1 || true
      rm -f "\$TMP"

      VTREF=\$(grep -oE 'VTref=[0-9.]+V' "\$LOG" | head -1 | sed 's/VTref=//' || echo "0V")
      HALTED=\$(grep -c "CPU could not be halted" "\$LOG" || true)
      DOWNLOAD=\$(grep -cE "Downloading file|Contents already match" "\$LOG" || true)
      OK=\$(grep -c "O\.K\." "\$LOG" || true)

      RAMCODE_ERR=\$(grep -c "RAMCode did not respond\|Unspecified error\|Writing target memory failed\|Failed to prepare" "\$LOG" || true)

      if [[ "\$OK" -gt 0 && "\$HALTED" -eq 0 && "\$DOWNLOAD" -gt 0 && "\$RAMCODE_ERR" -eq 0 ]]; then
          printf "\n${GREEN}[hil_flash_retry DONE]${NC} flashed ${APP} on attempt \$attempt/%s\n" "\$MAX_ATTEMPTS"
          exit 0
      fi

      if grep -q "Cannot connect to the probe" "\$LOG" 2>/dev/null; then
          printf "\r[attempt %3d] VTref=%-8s waiting for J-Link..." "\$attempt" "\${VTREF}"
      elif [[ "\$HALTED" -gt 0 ]]; then
          printf "\r[attempt %3d] VTref=%-8s locked -- retrying..." "\$attempt" "\${VTREF}"
      else
          printf "\r[attempt %3d] VTref=%-8s ..." "\$attempt" "\${VTREF}"
      fi

      sleep 0.1
done

printf "\n${RED}[hil_flash_retry FAIL]${NC} all %s attempts failed; log: %s\n" \
    "\$MAX_ATTEMPTS" "\$LOG" >&2
tail -25 "\$LOG" >&2
exit 1
REMOTE
else
  [[ "$-" == *p* ]]
fi
