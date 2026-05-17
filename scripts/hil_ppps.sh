#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# hil_ppps.sh -- Toggle the J-Link's USB port on/off via PPPS (Per-Port Power
# Switching) through uhubctl on the HIL Pi.  Triggers a USB re-enumeration
# without cutting board power.
#
# The VIA Labs hub is not a true PPPS hub: it can switch data/power per-port
# at the USB level but cannot cut the 5V rail to downstream devices.  Use
# hil_tapo.sh for a hard power cycle of the EK-RA8D2 board.
#
# J-Link location: hub 2-1.3 port 2 (USB 2.0) and hub 3-1.3 port 2 (USB 3.0).
# uhubctl toggles both simultaneously when addressed by the USB-2 hub path.
#
# Usage (run from repo root on dev machine):
#   bash scripts/hil_ppps.sh off
#   bash scripts/hil_ppps.sh on
#   bash scripts/hil_ppps.sh cycle     # off -> 1 s -> on
#
# Exit codes:
#   0  -- command succeeded
#   1  -- uhubctl error
#   2  -- usage error or Pi unreachable

set -euo pipefail

PI_HOST="star@star.local"
HUB="2-1.3"
PORT="2"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

[[ $# -eq 1 ]] || { echo "Usage: $0 <off|on|cycle>"; exit 2; }
CMD="$1"

ssh -o ConnectTimeout=5 -o BatchMode=yes "$PI_HOST" true 2>/dev/null \
    || { echo -e "${RED}[ERROR]${NC} cannot reach ${PI_HOST}"; exit 2; }

case "$CMD" in
    off)
        echo -e "${YELLOW}[hil_ppps]${NC} J-Link port OFF"
        ssh "$PI_HOST" "sudo -n uhubctl -l ${HUB} -p ${PORT} -a off" 2>&1
        ;;
    on)
        echo -e "${YELLOW}[hil_ppps]${NC} J-Link port ON"
        ssh "$PI_HOST" "sudo -n uhubctl -l ${HUB} -p ${PORT} -a on" 2>&1
        ;;
    cycle)
        echo -e "${YELLOW}[hil_ppps]${NC} J-Link port OFF"
        ssh "$PI_HOST" "sudo -n uhubctl -l ${HUB} -p ${PORT} -a off" 2>&1
        sleep 1
        echo -e "${YELLOW}[hil_ppps]${NC} J-Link port ON"
        ssh "$PI_HOST" "sudo -n uhubctl -l ${HUB} -p ${PORT} -a on" 2>&1
        ;;
    *)
        echo "Usage: $0 <off|on|cycle>"; exit 2
        ;;
esac

echo -e "${GREEN}[hil_ppps DONE]${NC} ${CMD}"
