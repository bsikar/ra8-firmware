#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# hil_ppps.sh -- Toggle a USB port on/off via PPPS (Per-Port Power Switching)
# through uhubctl on the HIL Pi.  Forces a USB re-enumeration of whatever
# is plugged into that hub port WITHOUT cutting board power.
#
# The VIA Labs hub at 2-1.3 is not a true PPPS hub: it switches data/power
# per-port at the USB level but cannot cut the 5V rail to downstream
# devices.  Use hil_tapo.sh for a hard power cycle of the EK-RA8D2 board.
#
# EK-RA8D2 wiring on hub 2-1.3 (see project_hil_wiring memory):
#   port 1 -> J7  USBHS  (1209:000c when HS firmware is running)
#   port 2 -> J-Link OB  (1366:1024 always)
#   port 4 -> J11 USBFS  (1209:000a when FS firmware is running)
#
# Usage (run from repo root on dev machine):
#   bash scripts/hil_ppps.sh <off|on|cycle> [port]
#     port defaults to 2 (J-Link).  Pick 1 for USBHS, 4 for USBFS.
#
# Exit codes:
#   0  -- command succeeded
#   1  -- uhubctl error
#   2  -- usage error or Pi unreachable

set -euo pipefail

PI_HOST="star@star.local"
HUB="2-1.3"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

[[ $# -ge 1 && $# -le 2 ]] || { echo "Usage: $0 <off|on|cycle> [port]"; exit 2; }
CMD="$1"
PORT="${2:-2}"

# Detect whether we are running on the Pi already (CI runner) so we skip SSH.
RUN_LOCAL=0
if [[ "$(hostname 2>/dev/null || true)" == "star" ]]; then
    RUN_LOCAL=1
fi

if (( RUN_LOCAL == 0 )); then
    ssh -o ConnectTimeout=5 -o BatchMode=yes "$PI_HOST" true 2>/dev/null \
        || { echo -e "${RED}[ERROR]${NC} cannot reach ${PI_HOST}"; exit 2; }
fi

run_uhubctl() {
    if (( RUN_LOCAL )); then
        sudo -n uhubctl -l "${HUB}" -p "${PORT}" -a "$1" 2>&1
    else
        ssh "$PI_HOST" "sudo -n uhubctl -l ${HUB} -p ${PORT} -a $1" 2>&1
    fi
}

case "$CMD" in
    off)
        echo -e "${YELLOW}[hil_ppps]${NC} port ${PORT} OFF"
        run_uhubctl off
        ;;
    on)
        echo -e "${YELLOW}[hil_ppps]${NC} port ${PORT} ON"
        run_uhubctl on
        ;;
    cycle)
        echo -e "${YELLOW}[hil_ppps]${NC} port ${PORT} OFF"
        run_uhubctl off
        sleep 1
        echo -e "${YELLOW}[hil_ppps]${NC} port ${PORT} ON"
        run_uhubctl on
        ;;
    *)
        echo "Usage: $0 <off|on|cycle> [port]"; exit 2
        ;;
esac

echo -e "${GREEN}[hil_ppps DONE]${NC} ${CMD} port ${PORT}"
