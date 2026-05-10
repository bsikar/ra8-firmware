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
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo -e "${YELLOW}[HIL]${NC} app=${APP_NAME}  expect='${EXPECT}'  timeout=${TIMEOUT_S}s"

# ---- 1. Strip OFS sections ---------------------------------------------------
# OFS sections at 0x0300A100+ cause J-Link RAMCode to timeout during Prepare()
# when TrustZone option bytes are involved.  Strip them so J-Link only programs
# the MRAM bank at 0x02000000.
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

# ---- 2. Flash via J-Link (loadfile with OFS-stripped hex) -------------------
# Direct w4 writes don't commit to MRAM cells without the MRMS flush sequence
# (MRCPC1 gate + MRCFLR flush per HUM Ch 59).  Implementing that in J-Link
# Commander is impractical, so we use loadfile (which uses RAMCode internally).
# OFS stripping above prevents the RAMCode Prepare() timeout that occurs when
# .option_setting* sections at 0x0300A100 are included.
#
# Pre-flash LPSCR clear (HUM Ch 11.2.18 / 11.2.20):
# Some apps (e.g. power_profiler) write LPSCR.LPMD = 0x4 (software standby)
# before WFI.  SYSRESETREQ via the debugger does NOT reset the SYSC LPM block
# (separate reset domain), so LPSCR survives reset.  When J-Link's RAMCode
# helper later executes any WFI, it gets trapped into software standby with
# no wake source -- "RAMCode did not respond" timeout cascades.  Clearing
# LPSCR via DAP (PRCR-unlock + write 0 + relock) makes RAMCode's WFI a plain
# Sleep that any interrupt can wake.
TMP_SCRIPT="$(mktemp)"
trap 'rm -f "$TMP_SCRIPT" "$STRIPPED_HEX"; pkill -f "cat ${UART}" 2>/dev/null || true' EXIT
cat > "$TMP_SCRIPT" <<JLINK
device ${JLINK_DEVICE}
si SWD
speed 1000
connect
halt
w2 0x4001E3FA 0xA502
w1 0x4001EA90 0x00
w2 0x4001E3FA 0xA500
loadfile ${STRIPPED_HEX}
r
g
q
JLINK

echo -e "${YELLOW}[HIL]${NC} flashing ${HEX}..."

# Start UART reader in the background BEFORE flashing.  The firmware's boot
# banner prints within milliseconds of "g" (go) -- if we open /dev/ttyACM0
# only after JLinkExe returns, one-shot boot banners are missed because the
# bytes arrive before any reader is attached.  Configure the tty first, then
# launch a background tail that streams to a log file we can grep afterward.
#
# Kill any lingering readers from a previous test that didn't clean up.
# Two cats on the same /dev/ttyACM0 each get only half the bytes, which
# silently breaks pattern matching for the next test.
pkill -f "cat ${UART}" 2>/dev/null || true
sleep 0.1
stty -F "${UART}" "${BAUD}" raw -echo
UART_LOG="/tmp/hil_uart_${APP_NAME}.log"
: > "${UART_LOG}"
# Use setsid so the cat does not share our session/process group -- this
# makes the cleanup pkill at end-of-script reliable regardless of exit path.
# stdbuf -o0 disables stdout buffering so every byte received from the tty
# is written to the log file immediately.  Without it, one-shot boot
# banners (e.g. "ulpt: wake\r\n" = 12 bytes) sit in cat's 4KB output buffer
# and grep races find an empty log.
setsid stdbuf -o0 cat "${UART}" > "${UART_LOG}" 2>/dev/null &
READER_PID=$!
# Make sure the reader actually opened the tty before we proceed.
sleep 0.2

# Single attempt: each loadfile op accumulates state in the MRAM controller
# (~13-op limit before PORST is required).  Retries make the accumulation
# worse without recovering from it, so we fail fast and let the suite move
# on; the user can power-cycle and rerun any failed apps.
JLinkExe -nogui 1 -SelectEmuBySN "${JLINK_SN}" -commanderscript "$TMP_SCRIPT" \
    > "${LOG_FILE}" 2>&1

if grep -qE "\*\*\*\*\*\* Error|Cannot connect to the probe|could not be halted|RAMCode did not respond" "${LOG_FILE}"; then
    kill "${READER_PID}" 2>/dev/null
    echo -e "${RED}[HIL]${NC} J-Link error -- log tail:" >&2
    tail -20 "${LOG_FILE}" >&2
    exit 1
fi
if ! grep -qE "Programming flash.*Done\.|Skipped\. Contents already match" "${LOG_FILE}"; then
    kill "${READER_PID}" 2>/dev/null
    echo -e "${RED}[HIL]${NC} flash phase missing -- log tail:" >&2
    tail -20 "${LOG_FILE}" >&2
    exit 1
fi
echo -e "${YELLOW}[HIL]${NC} flash OK"

# ---- 4. Wait for the expected string on UART  -----------------------------
# The background reader started before flashing has been capturing into
# ${UART_LOG} the whole time; tail-follow it until we see EXPECT or timeout.
echo -e "${YELLOW}[HIL]${NC} waiting for '${EXPECT}' on ${UART} (${TIMEOUT_S}s)..."

RESULT="TIMEOUT"
# Poll the log file every 100 ms for up to TIMEOUT_S seconds.  This is
# more robust than `tail -F | grep` which had a race where small one-shot
# prints (12-26 bytes) sat in cat's stdio buffer and were not visible to
# grep until cat was killed and flushed.
deadline=$(( SECONDS + TIMEOUT_S ))
while (( SECONDS < deadline )); do
    if grep -qF "${EXPECT}" "${UART_LOG}" 2>/dev/null; then
        RESULT="MATCH"
        break
    fi
    sleep 0.1
done
echo "--- captured UART ---"
sed 's/\r/\\r/g' "${UART_LOG}" | head -20 | sed 's/^/[uart] /'
echo "--- end ---"

# Stop the background tty reader.  It will keep running otherwise, consuming
# data from /dev/ttyACM0 and breaking the next test's reader.  setsid moved
# it out of our process group, so we pkill by name too as a safety net.
kill "${READER_PID}" 2>/dev/null || true
pkill -f "cat ${UART}" 2>/dev/null || true
wait "${READER_PID}" 2>/dev/null || true

if [[ "${RESULT}" == "MATCH" ]]; then
    echo -e "${GREEN}[HIL PASS]${NC} ${APP_NAME}: saw '${EXPECT}'"
    exit 0
else
    echo -e "${RED}[HIL FAIL]${NC} ${APP_NAME}: '${EXPECT}' not seen within ${TIMEOUT_S}s"
    exit 1
fi
