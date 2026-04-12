#!/bin/bash
# debug.sh -- Attach arm-none-eabi-gdb to a running EK-RA8D2 via J-Link GDB Server.
#
# Usage:
#   ./scripts/debug.sh                     # debug build/ra8d2-firmware.elf
#   ./scripts/debug.sh path/to/other.elf
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
FW_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

ELF="${1:-$FW_DIR/build/ra8d2-firmware.elf}"

if [[ ! -f "$ELF" ]]; then
    echo -e "${RED}Error:${NC} $ELF not found"
    echo "Build first with: ./build.sh"
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
echo -e "${GREEN}Starting JLinkGDBServer for R7KA8D2KF ...${NC}"
JLinkGDBServer -device R7KA8D2KF -if SWD -speed 4000 -port 2331 -nogui \
    > /tmp/jlinkgdbserver.log 2>&1 &
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
