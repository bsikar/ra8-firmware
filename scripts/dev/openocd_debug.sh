#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
# openocd_debug.sh -- Attach arm-none-eabi-gdb to a running EK-RA8D2 via
# OpenOCD's GDB server. GPL-tools alternative to debug.sh.
#
# Usage:
#   ./scripts/dev/openocd_debug.sh path/to/firmware.elf
#   ./scripts/dev/openocd_debug.sh                         # catalogue's blink app
#
# Starts openocd in the background (which exposes the GDB server on
# localhost:3333) and runs arm-none-eabi-gdb against the target.
# Press Ctrl-D at the gdb prompt to quit; openocd is cleaned up automatically.
#
# Requires:
#   - openocd 0.12+
#   - arm-none-eabi-gdb
#   - libusb access to the on-board J-Link OB

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
    ELF="$1"
  else
    blink_dir="$(python3 "$FW_DIR/scripts/dev/ra8_apps.py" dir blink)"
    blink_name="$(python3 "$FW_DIR/scripts/dev/ra8_apps.py" name blink)"
    ELF="$FW_DIR/$blink_dir/build/$blink_name.elf"
  fi

  # ---- bench mutual exclusion --------------------------------------------------
  # There is ONE EK-RA8D2 in this project; "attached to this machine" is where it
  # happens to be plugged in, not a second board. A debugger that halts the core
  # is every bit as disruptive to somebody else's run as a flash, so it takes the
  # same lock -- and holds it for the whole session, which is why the budget is
  # hours rather than minutes.
  # shellcheck source=scripts/hil/lib/bench_lock.sh
  source "$SCRIPT_DIR/../hil/lib/bench_lock.sh"
  ra8_bench_require "local OpenOCD gdb session" 2h || exit $?

  if [[ ! -f "$ELF" ]]; then
    echo -e "${RED}Error:${NC} $ELF not found"
    echo "Build first with: just apps::build <app>"
    exit 1
  fi

  if ! command -v openocd &>/dev/null; then
    echo -e "${RED}Error:${NC} openocd not found in PATH"
    exit 1
  fi

  if ! command -v arm-none-eabi-gdb &>/dev/null; then
    echo -e "${RED}Error:${NC} arm-none-eabi-gdb not found in PATH"
    exit 1
  fi

  # Inspect the exact ELF before GDB's `load` command programs it.
  # shellcheck source=scripts/hil/lib/preflash_guard.sh
  source "$SCRIPT_DIR/../hil/lib/preflash_guard.sh"
  ra8_preflash_guard "$ELF" || exit $?

  # Start the OpenOCD GDB server in the background. OpenOCD's default GDB
  # port is 3333, which is what we point gdb at below.
  echo -e "${GREEN}Starting OpenOCD GDB server on :3333 ...${NC}"
  openocd -f "$CFG" >/tmp/openocd.log 2>&1 &
  OCD_PID=$!

  cleanup() {
    if kill -0 "$OCD_PID" 2>/dev/null; then
      kill "$OCD_PID" 2>/dev/null || true
      wait "$OCD_PID" 2>/dev/null || true
    fi
  }
  trap cleanup EXIT INT TERM

  # Give the server a moment to come up.
  sleep 1

  arm-none-eabi-gdb "$ELF" \
    -ex "target extended-remote :3333" \
    -ex "monitor reset halt" \
    -ex "load" \
    -ex "monitor reset halt"
else
  [[ "$-" == *p* ]]
fi
