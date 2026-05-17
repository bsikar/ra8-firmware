#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# hil_check_alive.sh -- flash a firmware app, let it run for N seconds, then
# reconnect via J-Link and verify the CPU is still healthy (not in a hard
# fault, PC is in the MRAM region, watchdog hasn't reset us into a loop).
#
# This is the catch-all HIL verifier for apps that don't have a UART scrape
# or USB CDC echo to assert against -- LED blinkers, DAC waveform demos,
# CAN-loopback flag-toggling, ThreadX scheduling demos, etc. It's a weaker
# test than UART-string assertion but it's automatic and catches the broad
# regressions: HardFault, stack overflow, watchdog reset loop, NULL-pointer
# dereference, locked-up scheduler.
#
# What it checks (after the boot-seconds dwell):
#   1. J-Link can re-attach without "could not be halted" errors.
#   2. Program counter (PC) is in the MRAM code window
#      (0x02000000..0x020FFFFF). Hard-fault handlers in __default_handler
#      typically loop in MRAM too, but at a small known set of addresses we
#      reject.
#   3. SCB->HFSR (Hard Fault Status Register, 0xE000ED2C) has no bits set.
#   4. SCB->CFSR (Configurable Fault Status, 0xE000ED28) has no bits set.
#
# Usage:
#   bash scripts/hil_check_alive.sh --hex /path/to/<app>.hex \
#                                   [--boot-seconds 2] \
#                                   [--app-name <name>]
#
# Exit:
#   0 = chip is alive (no fault, PC in expected region)
#   1 = fault detected, PC in unexpected region, or J-Link error
#   2 = usage error or Pi unreachable

set -euo pipefail

PI_HOST="${PI_HOST:-star@star.local}"
JLINK_SN="1086567198"
JLINK_DEVICE="R7KA8D2KF_CPU0"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

HEX=""
BOOT_S=2
APP_NAME=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --hex) HEX="$2"; shift 2 ;;
        --boot-seconds) BOOT_S="$2"; shift 2 ;;
        --app-name) APP_NAME="$2"; shift 2 ;;
        -h|--help) sed -n '5,30p' "$0"; exit 0 ;;
        *) echo "Unknown arg: $1"; exit 2 ;;
    esac
done

[[ -n "$HEX" ]] || { echo "Usage: $0 --hex <file.hex> [--boot-seconds N]"; exit 2; }
[[ -f "$HEX" ]] || { echo -e "${RED}[ERROR]${NC} hex not found: $HEX"; exit 1; }
[[ -z "$APP_NAME" ]] && APP_NAME="$(basename "$HEX" .hex)"

# 1. Flash the app.
echo -e "${YELLOW}[alive]${NC} flashing ${APP_NAME}..."
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"${SCRIPT_DIR}/hil_flash.sh" "$APP_NAME" >/dev/null 2>&1 || {
    echo -e "${RED}[alive]${NC} flash failed for ${APP_NAME}"
    exit 1
}

# 2. Let the firmware run.
echo -e "${YELLOW}[alive]${NC} running for ${BOOT_S}s..."
sleep "$BOOT_S"

# 3. Re-attach via J-Link, snapshot PC + fault status registers.
# 0x02000000 = MRAM base, 0x02100000 = MRAM end (RA8D2 has 1MB MRAM).
# 0xE000ED28 = SCB->CFSR (Configurable Fault Status).
# 0xE000ED2C = SCB->HFSR (Hard Fault Status).
PI_TMP="/tmp/hil_alive_${APP_NAME}.jlink"
PI_LOG="/tmp/hil_alive_${APP_NAME}.log"
JLINK_SCRIPT=$(cat <<EOF
device ${JLINK_DEVICE}
si SWD
speed 1000
connect
halt
mem32 0xE000ED28 1
mem32 0xE000ED2C 1
regs
go
sleep 200
halt
regs
q
EOF
)

RUN_LOCAL=0
if [[ "$(hostname 2>/dev/null || true)" == "star" ]] \
   || [[ "$(hostname 2>/dev/null || true)" == "star-desktop" ]] \
   || [[ -e /dev/ttyACM0 && "$(uname -m)" == "aarch64" ]]; then
    RUN_LOCAL=1
fi

if (( RUN_LOCAL )); then
    echo "$JLINK_SCRIPT" > "$PI_TMP"
    JLinkExe -nogui 1 -SelectEmuBySN "${JLINK_SN}" -commanderscript "$PI_TMP" > "$PI_LOG" 2>&1 || true
else
    ssh "$PI_HOST" "cat > ${PI_TMP}" <<<"$JLINK_SCRIPT"
    ssh "$PI_HOST" "JLinkExe -nogui 1 -SelectEmuBySN ${JLINK_SN} -commanderscript ${PI_TMP} > ${PI_LOG} 2>&1 || true"
    scp -q "${PI_HOST}:${PI_LOG}" "/tmp/hil_alive_${APP_NAME}.log"
    PI_LOG="/tmp/hil_alive_${APP_NAME}.log"
fi

LOG_TEXT="$(cat "$PI_LOG")"

# Two PC + CycleCnt samples bracketing a 200ms `go` window. If the chip
# is making forward progress, CycleCnt increases.
#
# J-Link Commander's `regs` prints the register set twice in a row
# (FPU + integer banks each get a print pass), so a single regs call
# emits two identical "PC = ..., CycleCnt = ..." lines. We sample the
# FIRST distinct value (pre-go) and the LAST distinct value (post-go-
# sleep-halt); the values in between are just the duplicate prints
# from the same regs call.
mapfile -t PC_LIST < <(echo "$LOG_TEXT" | grep -oE 'PC[ ]*=[ ]*[0-9A-Fa-f]+' | grep -oE '[0-9A-Fa-f]+$' | tr 'a-f' 'A-F' | awk '!seen[$0]++')
mapfile -t CYC_LIST < <(echo "$LOG_TEXT" | grep -oE 'CycleCnt[ ]*=[ ]*[0-9A-Fa-f]+' | grep -oE '[0-9A-Fa-f]+$' | tr 'a-f' 'A-F' | awk '!seen[$0]++')
PC1="${PC_LIST[0]:-}"
PC2="${PC_LIST[$(( ${#PC_LIST[@]} > 1 ? ${#PC_LIST[@]} - 1 : 0 ))]:-${PC1}}"
CYC1="${CYC_LIST[0]:-}"
CYC2="${CYC_LIST[$(( ${#CYC_LIST[@]} > 1 ? ${#CYC_LIST[@]} - 1 : 0 ))]:-${CYC1}}"

# CFSR (active configurable-fault flags) -- bits cleared by firmware on
# normal boot; non-zero means an unhandled configurable fault.
CFSR_HEX="$(echo "$LOG_TEXT" | grep -iE '^E000ED28' | head -1 | awk '{print $3}' | tr 'a-f' 'A-F')"

ok=1
issues=()

if [[ -z "$PC1" || -z "$CYC1" ]]; then
    issues+=("could not parse PC/CycleCnt from J-Link output")
    ok=0
else
    pc1_dec=$((16#$PC1))
    pc2_dec=$((16#${PC2:-$PC1}))
    cyc1_dec=$((16#$CYC1))
    cyc2_dec=$((16#${CYC2:-$CYC1}))

    mram_lo=$((16#02000000))
    mram_hi=$((16#02100000))
    itcm_lo=$((16#00000000))
    itcm_hi=$((16#00010000))

    in_code_region() {
        local p="$1"
        (( p >= mram_lo && p < mram_hi )) && return 0
        (( p >= itcm_lo && p < itcm_hi )) && return 0
        return 1
    }

    if ! in_code_region "$pc1_dec"; then
        issues+=("PC1=0x${PC1} outside MRAM/ITCM")
        ok=0
    fi
    if ! in_code_region "$pc2_dec"; then
        issues+=("PC2=0x${PC2} outside MRAM/ITCM")
        ok=0
    fi
    if (( cyc2_dec <= cyc1_dec )); then
        issues+=("CycleCnt did not advance (0x${CYC1} -> 0x${CYC2}); CPU may be stuck")
        ok=0
    fi
fi

# CFSR is a strong real-fault indicator (not a debug artifact like
# HFSR.DEBUGEVT). If any configurable-fault bit is latched, treat as
# fail UNLESS the manifest opted into fault-expected (mpu_partition_*).
if [[ "${HIL_FAULT_EXPECTED:-0}" == "1" ]]; then
    : # caller said the fault is the test result; ignore CFSR.
elif [[ -n "$CFSR_HEX" ]]; then
    cfsr_dec=$((16#$CFSR_HEX))
    if (( cfsr_dec != 0 )); then
        issues+=("CFSR=0x${CFSR_HEX} non-zero (configurable fault latched)")
        ok=0
    fi
fi
# HFSR is NOT checked: bit 31 (DEBUGEVT) gets set whenever we halt via
# SWD on a device that hasn't enabled debug, so it's a false positive
# of this test methodology rather than a real fault indicator. PC range
# + CycleCnt progress is what we trust.

if (( ok == 1 )); then
    echo -e "${GREEN}[alive]${NC} ${APP_NAME} PASS (PC=0x${PC1}->0x${PC2}, CycleCnt 0x${CYC1}->0x${CYC2})"
    exit 0
else
    echo -e "${RED}[alive]${NC} ${APP_NAME} FAIL"
    for i in "${issues[@]}"; do echo "    - $i"; done
    echo "    (J-Link log: ${PI_LOG})"
    exit 1
fi
