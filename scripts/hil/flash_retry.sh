#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# hil_flash_retry.sh -- Power-cycle the EK-RA8D2 via uhubctl and flash an app.
#
# The board's USB is on hub 2-1 port 4 on the Pi.  uhubctl cuts VBUS,
# waits for the board to boot, then hammers JLinkExe until it catches
# the window before the firmware locks secure debug.
#
# Usage:
#   bash scripts/hil/flash_retry.sh <app>
#   bash scripts/hil/flash_retry.sh usb_cdc_echo

set -euo pipefail

# Rig config (PI_HOST, JLINK_SN) comes from the gitignored .env, not the tree.
_hil_dir="$(dirname "${BASH_SOURCE[0]}")"
_hil_dir="$(cd "$_hil_dir" && pwd)"
# shellcheck source=scripts/hil/lib/rig_env.sh
source "$_hil_dir/lib/rig_env.sh"
rig_require PI_HOST JLINK_SN
HUB="2-1"
PORT="4"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

[[ $# -eq 1 ]] || {
  echo "Usage: $0 <app>"
  exit 2
}
APP="$1"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

APP_DIR="$(find "$ROOT/examples" -name "main.c" |
  sed 's|/main\.c||' |
  while read -r d; do
    [[ "$(basename "$d")" == "$APP" ]] && echo "$d" || true
  done | head -1)"

[[ -n "$APP_DIR" ]] || {
  echo -e "${RED}[ERROR]${NC} app '${APP}' not found"
  exit 1
}

HEX="${APP_DIR}/build/${APP}.hex"
if [[ ! -f "$HEX" ]]; then
  echo -e "${YELLOW}[hil_flash_retry]${NC} building ${APP}..."
  make -C "$ROOT" "$APP"
fi
[[ -f "$HEX" ]] || {
  echo -e "${RED}[ERROR]${NC} build failed"
  exit 1
}

# ---- Anti-recovery pre-flash guard ------------------------------------------
# Inspect the full image + source tree before any programming. See
# scripts/hil/lib/preflash_guard.sh.
# shellcheck source=scripts/hil/lib/preflash_guard.sh
source "$_hil_dir/lib/preflash_guard.sh"
ra8_preflash_guard "$HEX" || exit $?

echo -e "${YELLOW}[hil_flash_retry]${NC} uploading hex to Pi..."
REMOTE_HEX="/tmp/hil_${APP}.hex"
scp -q "$HEX" "${PI_HOST}:${REMOTE_HEX}"

echo -e "${YELLOW}[hil_flash_retry]${NC} power-cycling board via uhubctl (hub ${HUB} port ${PORT})..."

# shellcheck disable=SC2087  # local vars (JLINK_SN, JLINK_DEVICE, etc.) expand client-side; remote vars are escaped
ssh "$PI_HOST" bash <<REMOTE
set -uo pipefail

JLINK_SN="${JLINK_SN}"
JLINK_DEVICE="${JLINK_DEVICE}"
REMOTE_HEX="${REMOTE_HEX}"
APP="${APP}"
HUB="${HUB}"
PORT="${PORT}"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

# ---- 1. Cut power (all 4 ports to ensure full board power-off) --------------
printf "${YELLOW}[hil_flash_retry]${NC} cutting power (all hub ports)...\n"
uhubctl -S -l "\$HUB" -a off > /dev/null 2>&1 || \
    sudo uhubctl -S -l "\$HUB" -a off > /dev/null 2>&1
sleep 1

# ---- 2. Restore power and hammer flash until window is caught ---------------
printf "${YELLOW}[hil_flash_retry]${NC} restoring power and flashing...\n"
uhubctl -S -l "\$HUB" -a on > /dev/null 2>&1 || \
    sudo uhubctl -S -l "\$HUB" -a on > /dev/null 2>&1

attempt=0
while true; do
    attempt=\$((attempt + 1))
    LOG="/tmp/hil_retry_\${APP}.log"

    TMP=\$(mktemp)
    cat > "\$TMP" <<JLINK
device \${JLINK_DEVICE}
si SWD
speed 4000
connect
halt
loadfile \${REMOTE_HEX}
r
g
q
JLINK

    JLinkExe -nogui 1 -SelectEmuBySN "\$JLINK_SN" -commanderscript "\$TMP" \
        > "\$LOG" 2>&1 || true
    rm -f "\$TMP"

    VTREF=\$(grep -oE 'VTref=[0-9.]+V' "\$LOG" | head -1 | sed 's/VTref=//' || echo "0V")
    HALTED=\$(grep -c "CPU could not be halted" "\$LOG" || true)
    DOWNLOAD=\$(grep -c "Downloading file" "\$LOG" || true)
    OK=\$(grep -c "O\.K\." "\$LOG" || true)

    RAMCODE_ERR=\$(grep -c "RAMCode did not respond\|Unspecified error\|Writing target memory failed\|Failed to prepare" "\$LOG" || true)

    if [[ "\$OK" -gt 0 && "\$HALTED" -eq 0 && "\$DOWNLOAD" -gt 0 && "\$RAMCODE_ERR" -eq 0 ]]; then
        printf "\n${GREEN}[hil_flash_retry DONE]${NC} flashed ${APP} on attempt \$attempt\n"
        exit 0
    fi

    if grep -q "Cannot connect to the probe" "\$LOG" 2>/dev/null; then
        printf "\r[attempt %3d] VTref=%-8s waiting for J-Link to enumerate..." \
            "\$attempt" "\${VTREF}"
    elif [[ "\$HALTED" -gt 0 ]]; then
        printf "\r[attempt %3d] VTref=%-8s locked -- retrying..." \
            "\$attempt" "\${VTREF}"
    else
        printf "\r[attempt %3d] VTref=%-8s ..." "\$attempt" "\${VTREF}"
    fi

    sleep 0.2
done
REMOTE
