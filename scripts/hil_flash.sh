#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# hil_flash.sh -- Build (optional) and flash a firmware app to the EK-RA8D2
# via the Pi HIL host over SSH.  No UART verification is performed; the board
# is simply programmed and released from reset.
#
# Usage (run from the repo root on the dev machine):
#   bash scripts/hil_flash.sh <app>
#   bash scripts/hil_flash.sh lcd_demo
#
# The hex is expected at:
#   examples/<tier>/.../<app>/build/<app>.hex
#
# If the hex is not found the script offers to build first.
#
# Exit codes:
#   0  -- flash succeeded
#   1  -- flash failed or hex not found
#   2  -- usage error or Pi unreachable

set -euo pipefail

PI_HOST="star@star.local"
JLINK_SN="1086567198"
JLINK_DEVICE="R7KA8D2KF_CPU0"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

[[ $# -eq 1 ]] || { echo "Usage: $0 <app>"; exit 2; }
APP="$1"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

APP_DIR="$(find "$ROOT/examples" -name "main.c" \
    | sed 's|/main\.c||' \
    | while read -r d; do
        [[ "$(basename "$d")" == "$APP" ]] && echo "$d" || true
    done | head -1)"

if [[ -z "$APP_DIR" ]]; then
    echo -e "${RED}[ERROR]${NC} app '${APP}' not found under examples/"
    exit 1
fi

HEX="${APP_DIR}/build/${APP}.hex"

if [[ ! -f "$HEX" ]]; then
    echo -e "${YELLOW}[hil_flash]${NC} hex not found: $HEX"
    echo -e "${YELLOW}[hil_flash]${NC} building ${APP}..."
    make -C "$ROOT" "$APP"
fi

[[ -f "$HEX" ]] || { echo -e "${RED}[ERROR]${NC} build failed -- $HEX still missing"; exit 1; }

echo -e "${YELLOW}[hil_flash]${NC} app=${APP}"

# ---- 1. Strip OFS sections ---------------------------------------------------
# OFS sections at 0x0300A100+ cause J-Link RAMCode to timeout during Prepare()
# when the CPU is in a TrustZone-locked state.  Strip them so J-Link only
# programs the MRAM bank at 0x02000000.
ELF="${APP_DIR}/build/${APP}.elf"
STRIPPED_HEX="/tmp/hil_${APP}_mram.hex"
OFS_ARGS=(
    --remove-section=.option_setting_ofs0  --remove-section=.option_setting_ofs1
    --remove-section=.option_setting_ofs2  --remove-section=.option_setting_ofs3
    --remove-section=.option_setting_ofs1_sec --remove-section=.option_setting_ofs1_sel
    --remove-section=.option_setting_ofs3_sec --remove-section=.option_setting_ofs3_sel
    --remove-section=.option_setting_bps0  --remove-section=.option_setting_bps1
    --remove-section=.option_setting_bps2  --remove-section=.option_setting_bps3
    --remove-section=.option_setting_pbps0 --remove-section=.option_setting_pbps1
    --remove-section=.option_setting_pbps2 --remove-section=.option_setting_pbps3
)
if [[ -f "$ELF" ]]; then
    arm-none-eabi-objcopy "${OFS_ARGS[@]}" -O ihex "$ELF" "$STRIPPED_HEX" 2>/dev/null \
        || cp "$HEX" "$STRIPPED_HEX"
else
    arm-none-eabi-objcopy -I ihex "${OFS_ARGS[@]}" -O ihex "$HEX" "$STRIPPED_HEX" 2>/dev/null \
        || cp "$HEX" "$STRIPPED_HEX"
fi

# ---- 2. Check Pi reachable ---------------------------------------------------
ssh -o ConnectTimeout=5 -o BatchMode=yes "$PI_HOST" true 2>/dev/null \
    || { echo -e "${RED}[ERROR]${NC} cannot reach ${PI_HOST}"; exit 2; }

# ---- 3. Copy hex to Pi -------------------------------------------------------
echo -e "${YELLOW}[hil_flash]${NC} uploading hex..."
REMOTE_HEX="/tmp/hil_${APP}.hex"
scp -q "$STRIPPED_HEX" "${PI_HOST}:${REMOTE_HEX}"

# ---- 4. Flash via J-Link on Pi -----------------------------------------------
echo -e "${YELLOW}[hil_flash]${NC} flashing..."
ssh "$PI_HOST" bash <<REMOTE
set -euo pipefail
TMP=\$(mktemp)
trap 'rm -f "\$TMP"' EXIT
cat > "\$TMP" <<JLINK
device ${JLINK_DEVICE}
si SWD
speed 4000
connect
halt
loadfile ${REMOTE_HEX}
r
g
q
JLINK
JLinkExe -nogui 1 -SelectEmuBySN ${JLINK_SN} -commanderscript "\$TMP" \
    > /tmp/hil_jlink_${APP}.log 2>&1
VTREF=\$(grep -oP 'VTref=\K[0-9.]+V' /tmp/hil_jlink_${APP}.log | head -1 || echo "unknown")
echo "    VTref : \${VTREF}"
echo "    log   : /tmp/hil_jlink_${APP}.log"
if grep -qiE "^Error|could not load|RAMCode did not respond|could not be halted" /tmp/hil_jlink_${APP}.log; then
    echo "---- J-Link log (errors detected) ----" >&2
    grep -iE "^Error|Warning|could not|failed|O\.K\.|VTref|Cortex|DAP|AP\[|loadfile|Downloading" \
        /tmp/hil_jlink_${APP}.log >&2 || cat /tmp/hil_jlink_${APP}.log >&2
    echo "---------------------------------------" >&2
    echo "(full log at /tmp/hil_jlink_${APP}.log on Pi)" >&2
    exit 1
fi
if ! grep -q "O.K." /tmp/hil_jlink_${APP}.log; then
    echo "---- J-Link log (no O.K. confirm) ----" >&2
    cat /tmp/hil_jlink_${APP}.log >&2
    echo "---------------------------------------" >&2
    exit 1
fi
REMOTE

echo -e "${GREEN}[hil_flash DONE]${NC} ${APP} is running on the board"
