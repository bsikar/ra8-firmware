#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# hil_erase.sh -- Mass-erase the EK-RA8D2 MRAM via J-Link over SSH to the Pi.
#
# Use this when the board is in a state where hil_flash.sh fails with
# "Failed to configure AP" or "CPU could not be halted".  Those errors
# mean the currently running firmware is holding the secure domain and
# blocking debug halt.  A mass erase wipes the firmware, leaving the
# board blank so the next hil_flash.sh invocation connects cleanly.
#
# Usage:
#   /bin/bash -p scripts/hil/erase.sh
#
# Exit codes:
#   0  -- erase confirmed
#   1  -- erase failed
#   2  -- Pi unreachable

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

  GREEN='\033[0;32m'
  RED='\033[0;31m'
  YELLOW='\033[1;33m'
  NC='\033[0m'

  tag() { printf "${YELLOW}[hil_erase]${NC} %s\n" "$*"; }
  ok() { printf "${GREEN}[OK]${NC}  %s\n" "$*"; }
  err() { printf "${RED}[FAIL]${NC} %s\n" "$*"; }

  # ---- bench mutual exclusion --------------------------------------------------
  # A mass erase is MORE destructive than a normal flash, not less, so it is not
  # exempt: an -erase-chip landing in the middle of somebody's suite is the worst
  # collision available. When the board is wedged the incumbent is usually dead
  # or is the one that wedged it, so there is a break-glass path -- see
  # ra8_bench_require_recovery.
  # shellcheck source=scripts/hil/lib/bench_lock.sh
  source "$_hil_dir/lib/bench_lock.sh"
  ra8_bench_require_recovery "mass-erase the MRAM" || exit $?

  tag "checking Pi ${PI_HOST}..."
  ssh -o ConnectTimeout=5 -o BatchMode=yes "$PI_HOST" true 2>/dev/null ||
    {
      err "cannot reach ${PI_HOST}"
      exit 2
    }
  ok "Pi reachable"

  tag "erasing MRAM (this takes ~5s)..."

  # shellcheck disable=SC2087  # local vars (JLINK_DEVICE, JLINK_SN) expand client-side; remote vars are escaped
  ssh "$PI_HOST" /bin/bash -p <<REMOTE
set -euo pipefail
TMP=\$(mktemp)
LOG=/tmp/hil_erase.log
trap 'rm -f "\$TMP"' EXIT

cat > "\$TMP" <<JLINK
device ${JLINK_DEVICE}
si SWD
speed 4000
connect
erase
q
JLINK

JLinkExe -nogui 1 -SelectEmuBySN ${JLINK_SN} -commanderscript "\$TMP" > "\$LOG" 2>&1 || true

echo "---- erase log ----"
grep -iE "erase|O\.K\.|error|warning|VTref|Cortex|complete|done|failed" "\$LOG" || cat "\$LOG"
echo "-------------------"

if grep -qiE "Erase done|Erasing done|O\.K\." "\$LOG"; then
    echo "ERASE_OK"
elif grep -q "Cortex-M85 identified" "\$LOG"; then
    echo "CONNECTED_NO_CONFIRM"
else
    echo "ERASE_FAIL"
fi
REMOTE

  # Re-run just to get the status token from the heredoc output
  STATUS=$(
    ssh "$PI_HOST" /bin/bash -p <<REMOTE2
LOG=/tmp/hil_erase.log
if grep -qiE "Erase done|Erasing done" "\$LOG" 2>/dev/null; then
    echo "OK"
elif grep -q "Cortex-M85 identified" "\$LOG" 2>/dev/null; then
    echo "PARTIAL"
else
    echo "FAIL"
fi
REMOTE2
  )

  case "$STATUS" in
    OK)
      ok "erase complete -- board is blank, safe to flash now"
      exit 0
      ;;
    PARTIAL)
      tag "connected but no erase confirmation -- trying to flash anyway may still work"
      tag "check full log on Pi: /tmp/hil_erase.log"
      exit 1
      ;;
    *)
      err "erase failed -- check /tmp/hil_erase.log on Pi"
      exit 1
      ;;
  esac
else
  [[ "$-" == *p* ]]
fi
