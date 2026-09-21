#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
# ozone.sh -- launch SEGGER Ozone debugger with a per-app ELF preloaded.
#
# Usage:
#   ./scripts/dev/ozone.sh path/to/firmware.elf       # debug a specific elf (preferred)
#   ./scripts/dev/ozone.sh                            # catalogue's blink app
#
# The Just hardware recipe invokes this with the selected app's .elf path
# (for example, `just apps::hardware::ozone blink_hal`).
#
# Requires:
#   - SEGGER Ozone installed (cask: segger-ozone)
#   - EK-RA8D2 plugged in via USB
#   - Firmware built (`just apps::example::build blink_hal` etc.)

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
  FW_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
  if [[ $# -gt 0 ]]; then
    ELF="$1"
  else
    blink_dir="$(python3 "$FW_DIR/scripts/dev/ra8_apps.py" dir blink)"
    blink_name="$(python3 "$FW_DIR/scripts/dev/ra8_apps.py" name blink)"
    ELF="$FW_DIR/$blink_dir/build/$blink_name.elf"
  fi

  OZONE_BIN=""
  for candidate in \
    "/Applications/SEGGER/Ozone/Ozone.app/Contents/MacOS/Ozone" \
    "/Applications/Ozone.app/Contents/MacOS/Ozone" \
    "$(command -v Ozone 2>/dev/null || true)"; do
    if [[ -n "${candidate}" && -x "${candidate}" ]]; then
      OZONE_BIN="${candidate}"
      break
    fi
  done

  if [[ -z "${OZONE_BIN}" ]]; then
    echo "error: Ozone not found." >&2
    echo "  install: brew install --cask segger-ozone" >&2
    exit 1
  fi

  if [[ ! -f "${ELF}" ]]; then
    echo "error: ${ELF} not found." >&2
    echo "  build: just apps::build <app>" >&2
    exit 1
  fi

  # Inspect the exact ELF before Ozone's File.Open action downloads it.
  # shellcheck source=scripts/hil/lib/preflash_guard.sh
  source "$SCRIPT_DIR/../hil/lib/preflash_guard.sh"
  ra8_preflash_guard "$ELF" || exit $?

  # Load the optional local probe serial without requiring a remote-rig host.
  # shellcheck source=scripts/hil/lib/rig_env.sh
  source "$SCRIPT_DIR/../hil/lib/rig_env.sh"
  ozone_serial="${JLINK_SN:-0}"

  # Generate the minimal project documented by SEGGER UM08025 rather than relying
  # on a missing, ignored maintainer-local .jdebug file. json.dumps produces a
  # quoted C-compatible string for paths containing spaces or backslashes.
  elf_literal="$(python3 -c 'import json,sys; print(json.dumps(sys.argv[1]))' "$ELF")"
  serial_literal="$(python3 -c 'import json,sys; print(json.dumps(sys.argv[1]))' "$ozone_serial")"
  JDEBUG="$(mktemp "${TMPDIR:-/tmp}/ra8d2_ozone.XXXXXX.jdebug")"
  trap 'rm -f "$JDEBUG" "${JDEBUG}.user"' EXIT
  cat >"$JDEBUG" <<JDEBUG_PROJECT
void OnProjectLoad (void) {
  Project.SetDevice ("R7KA8D2KF_CPU0");
  Project.SetHostIF ("USB", ${serial_literal});
  Project.SetTargetIF ("SWD");
  Project.SetTIFSpeed ("4 MHz");
  File.Open (${elf_literal});
}
JDEBUG_PROJECT

  # ---- bench mutual exclusion --------------------------------------------------
  # There is ONE EK-RA8D2 in this project; "attached to this machine" is where it
  # happens to be plugged in, not a second board. A debugger that halts the core
  # is every bit as disruptive to somebody else's run as a flash, so it takes the
  # same lock -- and holds it for the whole session, which is why the budget is
  # hours rather than minutes.
  # shellcheck source=scripts/hil/lib/bench_lock.sh
  source "$SCRIPT_DIR/../hil/lib/bench_lock.sh"
  ra8_bench_require "local SEGGER Ozone session" 2h || exit $?

  echo "==> Opening generated Ozone project for ${ELF}"
  "${OZONE_BIN}" "${JDEBUG}"
else
  [[ "$-" == *p* ]]
fi
