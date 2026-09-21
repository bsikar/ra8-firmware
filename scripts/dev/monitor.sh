#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
# monitor.sh -- Live UART serial monitor for the attached EK-RA8D2 board console.
#
# Usage:
#   ./scripts/dev/monitor.sh [BAUD]
#   just apps::hardware::monitor
#   just apps::hardware::monitor <app>

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
  BAUD="${1:-115200}"

  GREEN='\033[0;32m'
  CYAN='\033[0;36m'
  NC='\033[0m'

  # Source stable TTY resolver
  # shellcheck source=scripts/hil/lib/tty_resolve.sh
  source "$SCRIPT_DIR/../hil/lib/tty_resolve.sh"

  echo -e "${CYAN}=== Resolving EK-RA8D2 board console TTY ===${NC}"
  TTY_DEV="$(ra8_tty_resolve console)" || exit $?

  echo -e "${GREEN}=== Connecting to ${TTY_DEV} @ ${BAUD} baud ===${NC}"

  if command -v picocom &>/dev/null; then
    echo -e "${CYAN}Disconnect shortcut: Ctrl+A followed by Ctrl+X${NC}\n"
    exec picocom -b "$BAUD" "$TTY_DEV"
  elif command -v tio &>/dev/null; then
    echo -e "${CYAN}Disconnect shortcut: Ctrl+T followed by q${NC}\n"
    exec tio -b "$BAUD" "$TTY_DEV"
  elif command -v minicom &>/dev/null; then
    echo -e "${CYAN}Disconnect shortcut: Ctrl+A followed by x${NC}\n"
    exec minicom -D "$TTY_DEV" -b "$BAUD"
  elif python3 -c "import serial.tools.miniterm" &>/dev/null; then
    echo -e "${CYAN}Disconnect shortcut: Ctrl+]${NC}\n"
    exec python3 -m serial.tools.miniterm "$TTY_DEV" "$BAUD"
  else
    echo -e "${CYAN}Disconnect shortcut: Ctrl+C${NC}\n"
    stty -F "$TTY_DEV" "$BAUD" raw -echo
    exec cat "$TTY_DEV"
  fi
else
  [[ "$-" == *p* ]]
fi
