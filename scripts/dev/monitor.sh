#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# monitor.sh -- Live UART serial monitor for the attached EK-RA8D2 board console.
#
# Usage:
#   ./scripts/dev/monitor.sh [BAUD]
#   make monitor
#   make monitor-<app>

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BAUD="${1:-115200}"

GREEN='\033[0;32m'
CYAN='\033[0;36m'
NC='\033[0m'

# Source stable TTY resolver
# shellcheck disable=SC1091
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
