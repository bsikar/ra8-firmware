#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
# flash.sh -- Flash an Intel HEX firmware image to an attached EK-RA8D2 via SEGGER J-Link.
#
# Usage:
#   ./scripts/dev/flash.sh path/to/firmware.hex      # flash a specific file (preferred)
#   ./scripts/dev/flash.sh                           # flash the catalogue's blink app
#
# The Just hardware recipe invokes this with the selected app's .hex path
# (for example, `just apps::hardware::flash blink_hal`).
#
# Requires:
#   - J-Link software package with `JLinkExe` in PATH
#   - EK-RA8D2 plugged in (the on-board J-Link OB handles the SWD link)

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

  if [[ ! -f "$HEX" ]]; then
    echo -e "${RED}Error:${NC} $HEX not found"
    echo "Build first with: just apps::build <app>"
    exit 1
  fi

  # ---- bench mutual exclusion --------------------------------------------------
  # There is ONE EK-RA8D2 in this project. "Attached to this machine" is where it
  # happens to be plugged in today, not a different board -- so a local flash
  # takes the bench exactly like a remote one. If the bench host is unreachable
  # this fails closed, which is the correct answer: it cannot tell whether the
  # board is also being driven from the rig.
  # shellcheck source=scripts/hil/lib/bench_lock.sh
  source "$SCRIPT_DIR/../hil/lib/bench_lock.sh"
  ra8_bench_require "local J-Link flash of $(basename "$HEX")" || exit $?

  # ---- anti-recovery pre-flash guard ------------------------------------------
  # Inspect the image + source tree before programming; refuse any lockdown value
  # in the disable-initialize / DLM-lock / permanent-block-protect option region.
  # shellcheck source=scripts/hil/lib/preflash_guard.sh
  source "$SCRIPT_DIR/../hil/lib/preflash_guard.sh"
  ra8_preflash_guard "$HEX" || exit $?

  if ! command -v JLinkExe &>/dev/null; then
    echo -e "${RED}Error:${NC} JLinkExe not found in PATH"
    echo "Install the SEGGER J-Link package from https://www.segger.com/downloads/jlink/"
    exit 1
  fi

  echo -e "${GREEN}=== Flashing ${HEX} ===${NC}"

  TMP_SCRIPT=$(mktemp)
  trap 'rm -f "$TMP_SCRIPT"' EXIT

  # Notes on the device string:
  #   - `R7KA8D2KF_CPU0` is the JLink-recognised name for the EK-RA8D2 part
  #     (CPU0 = the Cortex-M85). This must be used instead of `CORTEX-M85`,
  #     because the generic core name causes JLink to bring up the device-
  #     selection GUI on macOS even with `-nogui 1`, and (worse) skips the
  #     RA-specific flash algorithm so MRAM writes silently fail.
  #   - `-nogui 1` suppresses the "Target device settings" picker.
  #   - We do NOT pin `-SelectEmuBySN`: the on-board J-Link OB's serial
  #     number changes when the USB cable or hub re-enumerates, and a
  #     stale pin silently "succeeds" (JLinkExe always exits 0) while
  #     actually skipping every command -- which means an old MRAM
  #     image keeps running and looks identical to a fresh flash.
  cat >"$TMP_SCRIPT" <<EOF
device R7KA8D2KF_CPU0
si SWD
speed 4000
connect
halt
loadfile $HEX
r
g
q
EOF

  LOG=$(mktemp)
  trap 'rm -f "$TMP_SCRIPT" "$LOG"' EXIT
  if ! JLinkExe -nogui 1 -commanderscript "$TMP_SCRIPT" | tee "$LOG"; then
    echo -e "${RED}[FAIL]${NC} JLinkExe returned a non-zero exit code"
    exit 1
  fi

  # JLinkExe always exits 0, even when the probe never connected.  Scan
  # the captured log for the connection-failure signature; bail out
  # loudly so a stale MRAM image cannot pass for a successful flash.
  if grep -qE 'Connecting to J-Link via USB\.\.\.FAILED|Cannot connect to J-Link' "$LOG"; then
    echo -e "${RED}[FAIL]${NC} J-Link probe not reachable -- nothing was flashed."
    echo "         Check the USB cable on the J-Link OB port (not the USB-Device port),"
    echo "         and confirm 'JLinkExe' alone can run 'connect' successfully."
    exit 1
  fi
  if grep -qE 'verify failed|Verify failed|Programming failed' "$LOG"; then
    echo -e "${RED}[FAIL]${NC} Flash verify failed -- MRAM image does not match the hex."
    exit 1
  fi

  echo -e "${GREEN}[DONE]${NC} Flashed $HEX"
else
  [[ "$-" == *p* ]]
fi
