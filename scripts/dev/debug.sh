#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# debug.sh -- Attach arm-none-eabi-gdb to a running EK-RA8D2 via J-Link GDB Server.
#
# Usage:
#   ./scripts/dev/debug.sh path/to/firmware.elf       # debug a specific elf (preferred)
#   ./scripts/dev/debug.sh                            # default: blink/build/blink.elf
#
# Per-app Makefiles invoke this with their own .elf path
# (e.g. `make -C blink_hal debug` -> `debug.sh blink_hal/build/blink_hal.elf`).
#
# Starts JLinkGDBServer in the background and then runs gdb against
# the target. Press Ctrl-D at the gdb prompt to quit; the gdb-server
# is cleaned up automatically.
#
# Requires:
#   - SEGGER J-Link package (`JLinkGDBServer`)
#   - ARM GNU Toolchain (`arm-none-eabi-gdb`)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FW_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Optional J-Link serial from the gitignored .env (see .env.example). Only
# needed to disambiguate multiple probes; single-probe machines can omit it.
# shellcheck source=scripts/hil/lib/rig_env.sh
source "$SCRIPT_DIR/../hil/lib/rig_env.sh"

GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

ELF="${1:-$FW_DIR/blink/build/blink.elf}"

if [[ ! -f "$ELF" ]]; then
  echo -e "${RED}Error:${NC} $ELF not found"
  echo "Build first with: 'make blink' (or 'make <app>')"
  exit 1
fi

if ! command -v JLinkGDBServer &>/dev/null; then
  echo -e "${RED}Error:${NC} JLinkGDBServer not found in PATH"
  exit 1
fi

if ! command -v arm-none-eabi-gdb &>/dev/null; then
  echo -e "${RED}Error:${NC} arm-none-eabi-gdb not found in PATH"
  exit 1
fi

# Start the GDB server in the background.
echo -e "${GREEN}Starting JLinkGDBServer for Cortex-M85 (RA8D2 fallback) ...${NC}"
# Use CORTEX-M85 instead of R7KA8D2KF to skip JLink's "unknown device" GUI
# (P/N R7KA8D2KFLCAC isn't in v9.38a's device DB).  -nogui drops the picker;
# -select USB=<SN> pins the on-board J-Link OB so multi-probe machines don't
# prompt either (set JLINK_SN in .env; skipped when unset).
SELECT_ARGS=()
[ -n "${JLINK_SN:-}" ] && SELECT_ARGS=(-select "USB=${JLINK_SN}")
JLinkGDBServer -device CORTEX-M85 -if SWD -speed 4000 -port 2331 -nogui \
  "${SELECT_ARGS[@]}" \
  >/tmp/jlinkgdbserver.log 2>&1 &
GDB_PID=$!

cleanup() {
  if kill -0 "$GDB_PID" 2>/dev/null; then
    kill "$GDB_PID" 2>/dev/null || true
    wait "$GDB_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

# Give the server a moment to come up.
sleep 1

# Run gdb with a scripted connect.
arm-none-eabi-gdb "$ELF" \
  -ex "target remote :2331" \
  -ex "monitor reset halt" \
  -ex "load" \
  -ex "monitor reset halt"
