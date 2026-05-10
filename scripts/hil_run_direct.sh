#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# hil_run_direct.sh -- Flash a firmware .hex and verify expected UART output.
#
# Intended for use DIRECTLY on the HIL host (Raspberry Pi 5) where the
# EK-RA8D2 board is physically wired.  This script has no SSH dependency --
# it runs JLinkExe and reads the UART locally.  For the developer-workstation
# variant that SSHes into the Pi, see scripts/hil_run.sh.
#
# Usage (run from the repo root on the Pi):
#   scripts/hil_run_direct.sh --hex <path/to/app.hex> \
#                             --expect <string>        \
#                             [--baud 115200]          \
#                             [--timeout 10]           \
#                             [--uart /dev/ttyACM0]
#
# Exit codes:
#   0  PASS  -- expected string appeared within timeout
#   1  FAIL  -- timeout elapsed without match, or flash failed
#   2  ERROR -- missing arguments / hardware not reachable

set -euo pipefail

JLINK_SN="1086567198"
JLINK_DEVICE="R7KA8D2KF_CPU0"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

usage() {
    echo "Usage: $0 --hex <file> --expect <string> [--baud 115200] [--timeout 10] [--uart /dev/ttyACM0]"
    exit 2
}

HEX=""
EXPECT=""
BAUD="115200"
TIMEOUT_S="10"
UART="/dev/ttyACM0"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --hex)     HEX="$2";       shift 2 ;;
        --expect)  EXPECT="$2";    shift 2 ;;
        --baud)    BAUD="$2";      shift 2 ;;
        --timeout) TIMEOUT_S="$2"; shift 2 ;;
        --uart)    UART="$2";      shift 2 ;;
        *) echo "Unknown arg: $1"; usage ;;
    esac
done

[[ -z "$HEX" || -z "$EXPECT" ]] && usage
[[ -f "$HEX" ]] || { echo -e "${RED}[HIL]${NC} hex not found: $HEX"; exit 2; }

APP_NAME="$(basename "${HEX%.hex}")"
LOG_FILE="/tmp/hil_jlink_${APP_NAME}.log"

echo -e "${YELLOW}[HIL]${NC} app=${APP_NAME}  expect='${EXPECT}'  timeout=${TIMEOUT_S}s"

# ---- 1. Strip OFS sections ---------------------------------------------------
# OFS sections at 0x0300A100+ cause J-Link RAMCode to timeout during Prepare()
# when the CPU is in a TrustZone-locked state.  Strip them so J-Link only
# programs the MRAM bank at 0x02000000.
ELF="${HEX%.hex}.elf"
STRIPPED_HEX="/tmp/hil_${APP_NAME}_mram.hex"
OFS_ARGS=( '--remove-section=.option_setting*' )
if [[ -f "$ELF" ]]; then
    arm-none-eabi-objcopy "${OFS_ARGS[@]}" -O ihex "$ELF" "$STRIPPED_HEX" 2>/dev/null \
        || cp "$HEX" "$STRIPPED_HEX"
else
    arm-none-eabi-objcopy -I ihex "${OFS_ARGS[@]}" -O ihex "$HEX" "$STRIPPED_HEX" 2>/dev/null \
        || cp "$HEX" "$STRIPPED_HEX"
fi

# ---- 2. Flash via J-Link -------------------------------------------------------
echo -e "${YELLOW}[HIL]${NC} flashing ${HEX}..."

TMP_SCRIPT="$(mktemp)"
trap 'rm -f "$TMP_SCRIPT" "$STRIPPED_HEX"' EXIT

cat > "$TMP_SCRIPT" <<JLINK
device ${JLINK_DEVICE}
si SWD
speed 4000
connect
halt
loadfile ${STRIPPED_HEX}
r
g
q
JLINK

JLinkExe -nogui 1 -SelectEmuBySN "${JLINK_SN}" -commanderscript "$TMP_SCRIPT" \
    > "${LOG_FILE}" 2>&1

if grep -qE "^Error|RAMCode did not respond|could not be halted|Cannot connect to the probe" "${LOG_FILE}"; then
    echo -e "${RED}[HIL]${NC} J-Link error -- log:" >&2
    cat "${LOG_FILE}" >&2
    exit 1
fi
echo -e "${YELLOW}[HIL]${NC} flash OK"

# ---- 2. Read UART and check for expected string --------------------------------
# stty configures the tty before reading. Feed the tty as stdin to a timed
# subshell that reads line-by-line; exit 0 the moment EXPECT appears.
echo -e "${YELLOW}[HIL]${NC} waiting for '${EXPECT}' on ${UART} (${TIMEOUT_S}s)..."

stty -F "${UART}" "${BAUD}" raw -echo

RESULT="TIMEOUT"
if timeout "${TIMEOUT_S}" bash -c "
    while IFS= read -r line; do
        printf '[uart] %s\n' \"\$line\"
        if printf '%s' \"\$line\" | grep -qF '${EXPECT}'; then
            exit 0
        fi
    done
    exit 1
" < "${UART}"; then
    RESULT="MATCH"
fi

if [[ "${RESULT}" == "MATCH" ]]; then
    echo -e "${GREEN}[HIL PASS]${NC} ${APP_NAME}: saw '${EXPECT}'"
    exit 0
else
    echo -e "${RED}[HIL FAIL]${NC} ${APP_NAME}: '${EXPECT}' not seen within ${TIMEOUT_S}s"
    exit 1
fi
