#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# openocd_flash.sh -- Flash an Intel HEX firmware image to an attached EK-RA8D2
# via OpenOCD + the on-board J-Link OB. GPL-tools alternative to flash.sh.
#
# Usage:
#   ./scripts/dev/openocd_flash.sh path/to/firmware.hex
#   ./scripts/dev/openocd_flash.sh                          # default: blink.hex
#
# Requires:
#   - openocd 0.12+ in PATH (Cortex-M85 CPUID recognition)
#   - libusb access to the on-board J-Link OB (udev rules on Linux)
#   - EK-RA8D2 plugged in
#
# See scripts/dev/openocd/ek-ra8d2.cfg for the chip / board config and TODOs
# around the MRAM flash driver.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FW_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
CFG="$SCRIPT_DIR/openocd/ek-ra8d2.cfg"

GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

HEX="${1:-$FW_DIR/examples/ek_ra8d2/blink/build/blink.hex}"

if [[ ! -f "$HEX" ]]; then
  echo -e "${RED}Error:${NC} $HEX not found"
  echo "Build first with: 'make blink' (or 'make <app>')"
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

echo -e "${GREEN}=== Flashing ${HEX} via OpenOCD ===${NC}"

openocd -f "$CFG" \
  -c "program $HEX verify reset exit"

echo -e "${GREEN}[DONE]${NC} Flashed $HEX"
