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
#   scripts/hil/run.sh --hex <path/to/app.elf|app.hex> \
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

# Rig config (PI_HOST, JLINK_SN) comes from the gitignored .env, not the tree.
_hil_dir="$(dirname "${BASH_SOURCE[0]}")"
_hil_dir="$(cd "$_hil_dir" && pwd)"
# shellcheck source=scripts/hil/lib/rig_env.sh
source "$_hil_dir/lib/rig_env.sh"
rig_require PI_HOST JLINK_SN
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
UART=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --hex)
      HEX="$2"
      shift 2
      ;;
    --expect)
      EXPECT="$2"
      shift 2
      ;;
    --baud)
      BAUD="$2"
      shift 2
      ;;
    --timeout)
      TIMEOUT_S="$2"
      shift 2
      ;;
    --uart)
      UART="$2"
      shift 2
      ;;
    *)
      echo "Unknown arg: $1"
      usage
      ;;
  esac
done

[[ -z "$HEX" || -z "$EXPECT" ]] && usage
[[ -f "$HEX" ]] || {
  echo -e "${RED}[HIL]${NC} firmware file not found: $HEX"
  exit 2
}

# Auto-detect J-Link OB CDC port on the Pi when --uart not given.  Without
# this the script falls back to /dev/ttyACM0, which is occupied by the
# chip's USBHS CDC device whenever a previous USB firmware ran (or even
# stale from before a reset), so reads return garbage from the wrong port.
# The J-Link OB CDC always reports ID_MODEL=J-Link via udev.
if [[ -z "$UART" ]]; then
  UART=$(
    ssh "$PI_HOST" bash -s <<'REMOTE'
for dev in /dev/ttyACM*; do
    [[ -e "$dev" ]] || continue
    if udevadm info "$dev" 2>/dev/null | grep -q "ID_MODEL=J-Link"; then
        echo "$dev"
        exit 0
    fi
done
echo "/dev/ttyACM0"
REMOTE
  )
  UART="${UART:-/dev/ttyACM0}"
fi

APP_NAME="$(basename "${HEX%.*}")"
FIRMWARE_EXT="${HEX##*.}"
REMOTE_FW="/tmp/hil_${APP_NAME}.${FIRMWARE_EXT}"

echo -e "${YELLOW}[HIL]${NC} app=${APP_NAME}  expect='${EXPECT}'  timeout=${TIMEOUT_S}s"

# ---- 1. Strip OFS sections ---------------------------------------------------
# OFS sections at 0x0300A100+ cause J-Link RAMCode to timeout during Prepare()
# when TrustZone option bytes are involved.  Strip them so J-Link only programs
# the MRAM bank at 0x02000000.
#
# SWD speed: RA8D2 boots from the internal ~4 MHz oscillator after SYSRESETREQ.
# J-Link's RAMCode runs at this low clock and requires speed <= 1000 kHz to
# communicate reliably.  Using 4000 kHz causes RAMCode timeout.
ELF="${HEX%.hex}.elf"
STRIPPED_FW="/tmp/hil_${APP_NAME}_mram.${FIRMWARE_EXT}"
OFS_ARGS=('--remove-section=.option_setting*')
if [[ "$FIRMWARE_EXT" == "elf" ]]; then
  arm-none-eabi-objcopy "${OFS_ARGS[@]}" -O ihex "$HEX" "/tmp/hil_${APP_NAME}_mram.hex" 2>/dev/null ||
    arm-none-eabi-objcopy -O ihex "$HEX" "/tmp/hil_${APP_NAME}_mram.hex"
  STRIPPED_FW="/tmp/hil_${APP_NAME}_mram.hex"
elif [[ -f "$ELF" ]]; then
  arm-none-eabi-objcopy "${OFS_ARGS[@]}" -O ihex "$ELF" "$STRIPPED_FW" 2>/dev/null ||
    cp "$HEX" "$STRIPPED_FW"
else
  arm-none-eabi-objcopy -I ihex "${OFS_ARGS[@]}" -O ihex "$HEX" "$STRIPPED_FW" 2>/dev/null ||
    cp "$HEX" "$STRIPPED_FW"
fi

# ---- 2. Copy firmware to Pi --------------------------------------------------
REMOTE_FW="/tmp/hil_${APP_NAME}_mram.hex"
echo -e "${YELLOW}[HIL]${NC} uploading hex..."
scp -q "$STRIPPED_FW" "${PI_HOST}:${REMOTE_FW}"

# ---- 3. Flash via J-Link on Pi -----------------------------------------------
echo -e "${YELLOW}[HIL]${NC} flashing..."
# shellcheck disable=SC2087  # client-side substitution of JLINK_DEVICE/JLINK_SN/REMOTE_FW/APP_NAME is intentional
ssh "$PI_HOST" bash <<REMOTE
set -euo pipefail
TMP_SCRIPT=\$(mktemp)
LOG=/tmp/hil_jlink_${APP_NAME}.log
trap 'rm -f "\$TMP_SCRIPT"' EXIT
cat > "\$TMP_SCRIPT" <<JLINK
device ${JLINK_DEVICE}
si SWD
speed 1000
connect
halt
loadfile ${REMOTE_FW}
r
g
q
JLINK
JLinkExe -nogui 1 -SelectEmuBySN ${JLINK_SN} -commanderscript "\$TMP_SCRIPT" > "\$LOG" 2>&1
if grep -qiE "^Error|could not load|RAMCode did not respond|could not be halted" "\$LOG"; then
    echo "J-Link error log:" >&2
    cat "\$LOG" >&2
    exit 1
fi
if ! grep -q "O\.K\." "\$LOG"; then
    echo "J-Link did not confirm download (no 'O.K.' in log):" >&2
    cat "\$LOG" >&2
    exit 1
fi
echo "flash OK"
REMOTE

# ---- 3. Read UART and check for expected string ------------------------------
echo -e "${YELLOW}[HIL]${NC} waiting for '${EXPECT}' on ${UART} (${TIMEOUT_S}s)..."
RESULT=$(
  # shellcheck disable=SC2087  # client-side substitution of UART/BAUD/TIMEOUT_S/EXPECT is intentional
  ssh "$PI_HOST" bash <<REMOTE
set -euo pipefail
# Configure baud rate once before opening the device for reading.
stty -F ${UART} ${BAUD} raw -echo cs8 -cstopb -parenb
# Pass the port as fd 3 to the child so the loop does not re-open it each line.
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
