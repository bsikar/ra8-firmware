#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# openocd_debug.sh -- Attach arm-none-eabi-gdb to a running EK-RA8D2 via
# OpenOCD's GDB server. GPL-tools alternative to debug.sh.
#
# Usage:
#   ./scripts/dev/openocd_debug.sh path/to/firmware.elf
#   ./scripts/dev/openocd_debug.sh                         # default: blink.elf
#
# Starts openocd in the background (which exposes the GDB server on
# localhost:3333) and runs arm-none-eabi-gdb against the target.
# Press Ctrl-D at the gdb prompt to quit; openocd is cleaned up automatically.
#
# Requires:
#   - openocd 0.12+
#   - arm-none-eabi-gdb
#   - libusb access to the on-board J-Link OB

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FW_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
CFG="$SCRIPT_DIR/openocd/ek-ra8d2.cfg"

GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

ELF="${1:-$FW_DIR/examples/ek_ra8d2/blink/build/blink.elf}"

if [[ ! -f "$ELF" ]]; then
  echo -e "${RED}Error:${NC} $ELF not found"
  echo "Build first with: 'make blink' (or 'make <app>')"
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
