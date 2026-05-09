#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# hil_run.sh -- Flash a firmware image (.elf or .hex) to the EK-RA8D2 via
# the Pi and verify expected output appears on the J-Link OB UART within a
# timeout.
#
# The Pi is the HIL host: the EK-RA8D2 board is physically wired to it,
# the J-Link OB is ttyACM0, and the board VCOM (SCI8) is also ttyACM0 at
# 115200 baud.
#
# Usage (run from the repo root on the dev machine):
#   scripts/hil_run.sh --hex <path/to/app.elf|app.hex> \
#                      --expect <string>                \
#                      [--baud 115200]                  \
#                      [--timeout 10]                   \
#                      [--uart /dev/ttyACM0]
#
# Exit codes:
#   0  PASS  -- expected string appeared within timeout
#   1  FAIL  -- timeout elapsed without match, or flash failed
#   2  ERROR -- missing arguments / unreachable Pi

set -euo pipefail

PI_HOST="star@star.local"
JLINK_SN="1086567198"
JLINK_DEVICE="R7KA8D2KF_CPU0"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

usage() {
    echo "Usage: $0 --hex <file.elf|file.hex> --expect <string> [--baud 115200] [--timeout 10] [--uart /dev/ttyACM0]"
    exit 2
}

HEX=""
EXPECT=""
BAUD="115200"
TIMEOUT_S="10"
UART="/dev/ttyACM0"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --hex)     HEX="$2";     shift 2 ;;
        --expect)  EXPECT="$2";  shift 2 ;;
        --baud)    BAUD="$2";    shift 2 ;;
        --timeout) TIMEOUT_S="$2"; shift 2 ;;
        --uart)    UART="$2";    shift 2 ;;
        *) echo "Unknown arg: $1"; usage ;;
    esac
done

[[ -z "$HEX" || -z "$EXPECT" ]] && usage
[[ -f "$HEX" ]] || { echo -e "${RED}[HIL]${NC} firmware file not found: $HEX"; exit 2; }

APP_NAME="$(basename "${HEX%.*}")"
FIRMWARE_EXT="${HEX##*.}"
REMOTE_FW="/tmp/hil_${APP_NAME}.${FIRMWARE_EXT}"

echo -e "${YELLOW}[HIL]${NC} app=${APP_NAME}  expect='${EXPECT}'  timeout=${TIMEOUT_S}s"

# ---- 1. Copy firmware to Pi --------------------------------------------------
echo -e "${YELLOW}[HIL]${NC} uploading ${FIRMWARE_EXT}..."
scp -q "$HEX" "${PI_HOST}:${REMOTE_FW}"

# ---- 2. Flash via J-Link on Pi -----------------------------------------------
echo -e "${YELLOW}[HIL]${NC} flashing..."
ssh "$PI_HOST" bash <<REMOTE
set -euo pipefail
TMP_SCRIPT=\$(mktemp)
trap 'rm -f "\$TMP_SCRIPT"' EXIT
cat > "\$TMP_SCRIPT" <<JLINK
device ${JLINK_DEVICE}
si 1
speed 4000
connect
r
halt
loadfile ${REMOTE_FW}
r
g
q
JLINK
JLinkExe -nogui 1 -SelectEmuBySN ${JLINK_SN} -commanderscript "\$TMP_SCRIPT" \
    > /tmp/hil_jlink_${APP_NAME}.log 2>&1
if grep -qiE "error|could not load|failed to" /tmp/hil_jlink_${APP_NAME}.log; then
    echo "J-Link error log:" >&2
    cat /tmp/hil_jlink_${APP_NAME}.log >&2
    exit 1
fi
if ! grep -q "O.K." /tmp/hil_jlink_${APP_NAME}.log; then
    echo "J-Link did not confirm download (no 'O.K.' in log):" >&2
    cat /tmp/hil_jlink_${APP_NAME}.log >&2
    exit 1
fi
echo "flash OK"
REMOTE

# ---- 3. Read UART and check for expected string ------------------------------
echo -e "${YELLOW}[HIL]${NC} waiting for '${EXPECT}' on ${UART} (${TIMEOUT_S}s)..."
RESULT=$(ssh "$PI_HOST" bash <<REMOTE
set -euo pipefail
# Configure baud rate once before opening the device for reading.
stty -F ${UART} ${BAUD} raw -echo cs8 -cstopb -parenb
# Open the port on a dedicated fd so the loop does not re-open it each line.
exec 3<>${UART}
timeout ${TIMEOUT_S} bash -c '
    while IFS= read -r line <&3; do
        echo "[uart] \$line"
        if echo "\$line" | grep -qF "${EXPECT}"; then
            echo "MATCH"
            exit 0
        fi
    done
    exit 1
' 3<>${UART} && echo "FOUND" || echo "TIMEOUT"
REMOTE
)

echo -e "${YELLOW}[HIL]${NC} output: ${RESULT}"

if echo "$RESULT" | grep -q "FOUND\|MATCH"; then
    echo -e "${GREEN}[HIL PASS]${NC} ${APP_NAME}: saw '${EXPECT}'"
    exit 0
else
    echo -e "${RED}[HIL FAIL]${NC} ${APP_NAME}: '${EXPECT}' not seen within ${TIMEOUT_S}s"
    exit 1
fi
