#!/bin/bash
# flash.sh -- Flash ra8d2-firmware.hex to an attached EK-RA8D2 via SEGGER J-Link.
#
# Usage:
#   ./scripts/flash.sh                          # flash build/ra8d2-firmware.hex
#   ./scripts/flash.sh build/other.hex          # flash a specific file
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

HEX="${1:-$FW_DIR/build/ra8d2-firmware.hex}"

if [[ ! -f "$HEX" ]]; then
    echo -e "${RED}Error:${NC} $HEX not found"
    echo "Build first with: ./build.sh"
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

cat > "$TMP_SCRIPT" <<EOF
device R7KA8D2KF
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

JLinkExe -commanderscript "$TMP_SCRIPT"

echo -e "${GREEN}[DONE]${NC} Flashed $HEX"
