#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
# openocd_flash.sh -- Flash an Intel HEX firmware image to an attached EK-RA8D2
# via OpenOCD + the on-board J-Link OB. GPL-tools alternative to flash.sh.
#
# Usage:
#   ./scripts/dev/openocd_flash.sh path/to/firmware.hex
#   ./scripts/dev/openocd_flash.sh                          # catalogue's blink app
#
# Requires:
#   - openocd 0.12+ in PATH (Cortex-M85 CPUID recognition)
#   - libusb access to the on-board J-Link OB (udev rules on Linux)
#   - EK-RA8D2 plugged in
#
# See scripts/dev/openocd/ek-ra8d2.cfg for the chip / board config and TODOs
# around the MRAM flash driver.

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

  SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  FW_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
  CFG="$SCRIPT_DIR/openocd/ek-ra8d2.cfg"

  GREEN='\033[0;32m'
  RED='\033[0;31m'
  NC='\033[0m'

  if [[ $# -gt 0 ]]; then
    HEX="$1"
  else
    blink_dir="$(python3 "$FW_DIR/scripts/dev/ra8_apps.py" dir blink)"
    blink_name="$(python3 "$FW_DIR/scripts/dev/ra8_apps.py" name blink)"
    HEX="$FW_DIR/$blink_dir/build/$blink_name.hex"
  fi

  # ---- bench mutual exclusion --------------------------------------------------
  # There is ONE EK-RA8D2 in this project; "attached to this machine" is where it
  # happens to be plugged in, not a second board. A debugger that halts the core
  # is every bit as disruptive to somebody else's run as a flash, so it takes the
  # same lock -- and holds it for the whole session, which is why the budget is
  # hours rather than minutes.
  # shellcheck source=scripts/hil/lib/bench_lock.sh
  source "$SCRIPT_DIR/../hil/lib/bench_lock.sh"
  ra8_bench_require "local OpenOCD flash" || exit $?

  if [[ ! -f "$HEX" ]]; then
    echo -e "${RED}Error:${NC} $HEX not found"
    echo "Build first with: just apps::build <app>"
    exit 1
  fi

  if [[ ! -f "$CFG" ]]; then
    echo -e "${RED}Error:${NC} OpenOCD config $CFG not found"
    exit 1
  fi

  if ! command -v openocd &>/dev/null; then
    echo -e "${RED}Error:${NC} openocd not found in PATH"
    echo "Install: 'sudo apt install openocd' (Linux) or 'brew install openocd' (macOS)"
    exit 1
  fi

  # Inspect the exact image before OpenOCD's `program` command writes it.
  # shellcheck source=scripts/hil/lib/preflash_guard.sh
  source "$SCRIPT_DIR/../hil/lib/preflash_guard.sh"
  ra8_preflash_guard "$HEX" || exit $?

  echo -e "${GREEN}=== Flashing ${HEX} via OpenOCD ===${NC}"

  openocd -f "$CFG" \
    -c "program $HEX verify reset exit"

  echo -e "${GREEN}[DONE]${NC} Flashed $HEX"
else
  [[ "$-" == *p* ]]
fi
