#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# hil_usb_test.sh -- HIL test runner for both USBFS and USBHS CDC echo on
# the EK-RA8D2.  Designed to run on the Pi (star@star.local) as the
# self-hosted gitea/github runner, or on a dev machine that can SSH there.
#
# What it does, for each controller:
#   1. Flash the corresponding "tz_secure_only_usb*" app via the J-Link.
#   2. Power-cycle the board with hil_tapo.sh so the new firmware boots.
#   3. Wait for the 1209:xxx CDC ACM device to appear on the right hub port.
#   4. Run scripts/usb_benchmark.py against /dev/ttyACMx for correctness
#      + throughput.
#   5. PPPS-cycle the hub port to force USB re-enumeration without cutting
#      board power; assert the device reappears.
#
# Usage (run from the repo root):
#   bash scripts/hil_usb_test.sh                  # both FS and HS
#   bash scripts/hil_usb_test.sh --only fs
#   bash scripts/hil_usb_test.sh --only hs
#   bash scripts/hil_usb_test.sh --quick          # short correctness only
#
# Exit codes:
#   0 -- every selected test passed
#   1 -- at least one test failed
#   2 -- usage error or cannot reach the HIL Pi

set -euo pipefail

PI_HOST="star@star.local"
HUB="2-1.3"
HUB_PORT_USBHS=1
HUB_PORT_USBFS=4
ENUM_WAIT_S=15
PPPS_REENUM_RETRIES=3

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

ONLY=""
QUICK=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --only)  ONLY="$2"; shift 2 ;;
        --quick) QUICK="--quick"; shift ;;
        -h|--help) sed -n '6,28p' "$0"; exit 0 ;;
        *) echo "Unknown arg: $1"; exit 2 ;;
    esac
done

ssh -o ConnectTimeout=5 -o BatchMode=yes "$PI_HOST" true 2>/dev/null \
    || { echo -e "${RED}[ERROR]${NC} cannot reach ${PI_HOST}"; exit 2; }

# Make sure the Pi has the latest copy of usb_benchmark.py.
scp -q scripts/usb_benchmark.py "${PI_HOST}:/tmp/usb_benchmark.py"

# Wait until lsusb reports the given VID:PID on the right hub port.
wait_for_enum() {
    local vidpid="$1" port="$2" timeout="${3:-$ENUM_WAIT_S}" t=0
    while (( t < timeout )); do
        if ssh "$PI_HOST" "lsusb 2>/dev/null | grep -q '${vidpid}'" 2>/dev/null; then
            return 0
        fi
        sleep 1
        (( t++ ))
    done
    return 1
}

run_one_controller() {
    local name="$1" app="$2" vidpid="$3" hub_port="$4"
    local rc=0

    echo
    echo -e "${CYAN}===========================================================${NC}"
    echo -e "${CYAN}[USB-${name}]${NC} app=${app}  vidpid=${vidpid}  hub_port=${hub_port}"
    echo -e "${CYAN}===========================================================${NC}"

    echo -e "${YELLOW}[USB-${name}]${NC} step 1 -- flash"
    bash scripts/hil_flash.sh "${app}" >/dev/null

    echo -e "${YELLOW}[USB-${name}]${NC} step 2 -- hard power cycle (Tapo)"
    bash scripts/hil_tapo.sh cycle >/dev/null

    echo -e "${YELLOW}[USB-${name}]${NC} step 3 -- wait for enumeration"
    if wait_for_enum "${vidpid}" "${hub_port}" "${ENUM_WAIT_S}"; then
        echo -e "${GREEN}[USB-${name}]${NC} enumerated"
    else
        echo -e "${RED}[USB-${name}]${NC} did NOT enumerate after power-cycle"
        return 1
    fi

    echo -e "${YELLOW}[USB-${name}]${NC} step 4 -- run usb_benchmark.py"
    if ssh "$PI_HOST" "python3 /tmp/usb_benchmark.py ${QUICK}"; then
        echo -e "${GREEN}[USB-${name}]${NC} benchmark PASS"
    else
        echo -e "${RED}[USB-${name}]${NC} benchmark FAIL"
        rc=1
    fi

    echo -e "${YELLOW}[USB-${name}]${NC} step 5 -- PPPS re-enumeration on port ${hub_port}"
    local reenum_ok=0
    for attempt in $(seq 1 "${PPPS_REENUM_RETRIES}"); do
        bash scripts/hil_ppps.sh cycle "${hub_port}" >/dev/null 2>&1
        if wait_for_enum "${vidpid}" "${hub_port}" "${ENUM_WAIT_S}"; then
            echo -e "${GREEN}[USB-${name}]${NC} PPPS re-enumeration OK (attempt ${attempt})"
            reenum_ok=1
            break
        fi
        echo -e "${YELLOW}[USB-${name}]${NC} PPPS re-enum attempt ${attempt} failed; retrying"
    done
    if (( reenum_ok == 0 )); then
        echo -e "${RED}[USB-${name}]${NC} PPPS re-enumeration FAILED after ${PPPS_REENUM_RETRIES} tries"
        rc=1
    fi

    return $rc
}

overall=0

if [[ -z "$ONLY" || "$ONLY" == "fs" ]]; then
    run_one_controller "FS" "tz_secure_only_usb"    "1209:000a" "$HUB_PORT_USBFS" || overall=1
fi

if [[ -z "$ONLY" || "$ONLY" == "hs" ]]; then
    run_one_controller "HS" "tz_secure_only_usb_hs" "1209:000c" "$HUB_PORT_USBHS" || overall=1
fi

echo
if (( overall == 0 )); then
    echo -e "${GREEN}[hil_usb_test DONE]${NC} all selected USB tests PASSED"
else
    echo -e "${RED}[hil_usb_test DONE]${NC} at least one USB test FAILED"
fi
exit $overall
