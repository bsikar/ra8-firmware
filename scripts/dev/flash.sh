#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# flash.sh -- Flash an Intel HEX firmware image to an attached EK-RA8D2 via SEGGER J-Link.
#
# Usage:
#   ./scripts/dev/flash.sh path/to/firmware.hex      # flash a specific file (preferred)
#   ./scripts/dev/flash.sh                           # flash blink/build/blink.hex (the default app)
#
# Per-app Makefiles invoke this with their own .hex path
# (e.g. `make -C blink_hal flash` -> `flash.sh blink_hal/build/blink_hal.hex`).
#
# Requires:
#   - J-Link software package with `JLinkExe` in PATH
#   - EK-RA8D2 plugged in (the on-board J-Link OB handles the SWD link)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FW_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

HEX="${1:-$FW_DIR/examples/ek_ra8d2/blink/build/blink.hex}"

if [[ ! -f "$HEX" ]]; then
  echo -e "${RED}Error:${NC} $HEX not found"
  echo "Build first with: 'make blink' (or 'make <app>')"
  exit 1
fi

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
