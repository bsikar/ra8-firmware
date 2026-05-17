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
# when TrustZone option bytes are involved.  Strip them so J-Link only programs
# the MRAM bank at 0x02000000.
#
# SWD speed: RA8D2 boots from the internal ~4 MHz oscillator after SYSRESETREQ.
# J-Link's RAMCode runs at this low clock and requires speed <= 1000 kHz to
# communicate reliably.  Using 4000 kHz causes RAMCode timeout.
ELF="${APP_DIR}/build/${APP}.elf"
STRIPPED_HEX="/tmp/hil_${APP}_mram.hex"
OFS_ARGS=( '--remove-section=.option_setting*' )
if [[ -f "$ELF" ]]; then
    arm-none-eabi-objcopy "${OFS_ARGS[@]}" -O ihex "$ELF" "$STRIPPED_HEX" 2>/dev/null \
        || cp "$HEX" "$STRIPPED_HEX"
else
    arm-none-eabi-objcopy -I ihex "${OFS_ARGS[@]}" -O ihex "$HEX" "$STRIPPED_HEX" 2>/dev/null \
        || cp "$HEX" "$STRIPPED_HEX"
fi

# Detect if we are already on the Pi (CI self-hosted runner case).
RUN_LOCAL=0
if [[ "$(hostname 2>/dev/null || true)" == "star" ]] \
   || [[ "$(hostname 2>/dev/null || true)" == "star-desktop" ]] \
   || [[ -e /dev/ttyACM0 && "$(uname -m)" == "aarch64" ]]; then
    RUN_LOCAL=1
fi

REMOTE_HEX="/tmp/hil_${APP}_mram.hex"
LOG="/tmp/hil_jlink_${APP}.log"

if (( RUN_LOCAL == 0 )); then
    # ---- 2. Check Pi reachable -----------------------------------------------
    ssh -o ConnectTimeout=5 -o BatchMode=yes "$PI_HOST" true 2>/dev/null \
        || { echo -e "${RED}[ERROR]${NC} cannot reach ${PI_HOST}"; exit 2; }

    # ---- 3. Copy hex to Pi ---------------------------------------------------
    echo -e "${YELLOW}[hil_flash]${NC} uploading hex..."
    scp -q "$STRIPPED_HEX" "${PI_HOST}:${REMOTE_HEX}"
else
    cp "$STRIPPED_HEX" "$REMOTE_HEX"
fi

# ---- 4. Flash via J-Link (local on Pi, or via SSH) ---------------------------
echo -e "${YELLOW}[hil_flash]${NC} flashing..."
flash_cmds() {
    cat <<EOF
device ${JLINK_DEVICE}
si SWD
speed 1000
connect
halt
loadfile ${REMOTE_HEX}
r
g
q
EOF
}
post_check() {
    local log="$1"
    VTREF=$(grep -oP 'VTref=\K[0-9.]+V' "$log" | head -1 || echo "unknown")
    echo "    VTref : ${VTREF}"
    echo "    log   : ${log}"
    if grep -qiE "^Error|could not load|RAMCode did not respond|could not be halted" "$log"; then
        echo "---- J-Link log (errors detected) ----" >&2
        grep -iE "^Error|Warning|could not|failed|O\.K\.|VTref|Cortex|DAP|AP\[|loadfile|Downloading" \
            "$log" >&2 || cat "$log" >&2
        echo "---------------------------------------" >&2
        return 1
    fi
    if ! grep -q "O\.K\." "$log"; then
        echo "---- J-Link log (no O.K. confirm) ----" >&2
        cat "$log" >&2
        echo "---------------------------------------" >&2
        return 1
    fi
    return 0
}

if (( RUN_LOCAL )); then
    TMP=$(mktemp)
    trap 'rm -f "$TMP"' EXIT
    flash_cmds > "$TMP"
    JLinkExe -nogui 1 -SelectEmuBySN "${JLINK_SN}" -commanderscript "$TMP" > "$LOG" 2>&1
    post_check "$LOG" || exit 1
else
    ssh "$PI_HOST" bash <<REMOTE
set -euo pipefail
TMP=\$(mktemp)
LOG="${LOG}"
trap 'rm -f "\$TMP"' EXIT
cat > "\$TMP" <<JLINK
$(flash_cmds)
JLINK
JLinkExe -nogui 1 -SelectEmuBySN ${JLINK_SN} -commanderscript "\$TMP" > "\$LOG" 2>&1
VTREF=\$(grep -oP 'VTref=\K[0-9.]+V' "\$LOG" | head -1 || echo "unknown")
echo "    VTref : \${VTREF}"
echo "    log   : \${LOG}"
if grep -qiE "^Error|could not load|RAMCode did not respond|could not be halted" "\$LOG"; then
    echo "---- J-Link log (errors detected) ----" >&2
    grep -iE "^Error|Warning|could not|failed|O\.K\.|VTref|Cortex|DAP|AP\[|loadfile|Downloading" \
        "\$LOG" >&2 || cat "\$LOG" >&2
    echo "---------------------------------------" >&2
    echo "(full log at \${LOG} on Pi)" >&2
    exit 1
fi
if ! grep -q "O\.K\." "\$LOG"; then
    echo "---- J-Link log (no O.K. confirm) ----" >&2
    cat "\$LOG" >&2
    echo "---------------------------------------" >&2
    exit 1
fi
REMOTE
fi

echo -e "${GREEN}[hil_flash DONE]${NC} ${APP} is running on the board"
