#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# flash.sh -- Flash an Intel HEX firmware image to an attached EK-RA8D2 via SEGGER J-Link.
#
# Usage:
#   ./scripts/flash.sh path/to/firmware.hex      # flash a specific file (preferred)
#   ./scripts/flash.sh                           # flash blink/build/blink.hex (the default app)
#
# Per-app Makefiles invoke this with their own .hex path
# (e.g. `make -C blink_hal flash` -> `flash.sh blink_hal/build/blink_hal.hex`).
#
# Requires:
#   - J-Link software package with `JLinkExe` in PATH
#   - EK-RA8D2 plugged in (the on-board J-Link OB handles the SWD link)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FW_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
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
#   - `-SelectEmuBySN` pins the probe so multi-probe machines don't prompt.
cat > "$TMP_SCRIPT" <<EOF
device R7KA8D2KF_CPU0
si 1
speed 4000
connect
r
halt
loadfile $HEX
r
g
q
EOF

JLinkExe -nogui 1 -SelectEmuBySN 1086567198 -commanderscript "$TMP_SCRIPT"

echo -e "${GREEN}[DONE]${NC} Flashed $HEX"
