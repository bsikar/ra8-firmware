#!/bin/bash
# hw_smoke_test.sh -- Hardware smoke-test harness for EK-RA8D2 (EVM tier).
#
# Iterates every app under examples/ek_ra8d2/, flashes the prebuilt .hex via
# scripts/flash.sh, halts the chip via JLinkExe, reads the program counter,
# resolves it through arm-none-eabi-addr2line, and classifies the result as
# PASS / WIP / FAIL / NOBUILD.
#
# Classification rules (matches docs/HARDWARE_BRINGUP.md):
#   PASS    PC resolves into tx_thread_schedule.S, ra_time.c (ra_delay_ms
#           busy-wait), or any user main loop / internal_* idle that is not
#           a panic_halt sink.
#   WIP     PC parked in panic_halt / demo_panic_halt / usb_hid_panic_halt /
#           usb_msc_panic_halt / internal_ra_fatal_error -- the firmware
#           reached its own caught-error sink (init failed cleanly).
#   FAIL    PC == 0xEFFFFFFE (Cortex-M lockup) or in Default_Handler /
#           HardFault_Handler / MemManage_Handler / BusFault_Handler /
#           UsageFault_Handler / SecureFault_Handler.
#   NOBUILD The app's .elf or .hex is missing -- caller forgot to `make <app>`.
#
# Usage:
#   bash scripts/hw_smoke_test.sh                       # sweep every EVM app
#   bash scripts/hw_smoke_test.sh --apps "blink uart_hello"  # subset
#   bash scripts/hw_smoke_test.sh --settle 8            # wait 8s post-flash
#   bash scripts/hw_smoke_test.sh --dry-run             # skip JLink, mock PC
#
# Outputs:
#   build/smoke/results.md   -- aggregate Markdown summary table
#   build/smoke/<app>.log    -- per-app PC, LR, JLinkExe transcript
#
# Exit codes:
#   0  every app PASS or WIP
#   1  at least one app FAIL
#   2  invocation error (missing tools, bad args)
#
# Requires:
#   JLinkExe                  in PATH (SEGGER J-Link package)
#   arm-none-eabi-addr2line   in PATH (ARM GNU toolchain)
#   EK-RA8D2 board attached via the on-board J-Link OB
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TIER_DIR="$ROOT_DIR/examples/ek_ra8d2"
OUT_DIR="$ROOT_DIR/build/smoke"
RESULTS_MD="$OUT_DIR/results.md"

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
BLUE='\033[0;34m'
NC='\033[0m'

SETTLE_S=5
DRY_RUN=0
APPS_FILTER=""

usage() {
    sed -n '2,40p' "$0"
}

# ---------- argument parsing ----------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --apps)
            APPS_FILTER="${2:-}"
            shift 2
            ;;
        --settle)
            SETTLE_S="${2:-5}"
            shift 2
            ;;
        --dry-run)
            DRY_RUN=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage
            exit 2
            ;;
    esac
done

# ---------- tool checks ----------
if [[ "$DRY_RUN" -eq 0 ]]; then
    if ! command -v JLinkExe >/dev/null 2>&1; then
        echo -e "${RED}Error:${NC} JLinkExe not found in PATH" >&2
        exit 2
    fi
fi
if ! command -v arm-none-eabi-addr2line >/dev/null 2>&1; then
    echo -e "${RED}Error:${NC} arm-none-eabi-addr2line not found in PATH" >&2
    exit 2
fi

mkdir -p "$OUT_DIR"

# ---------- discover apps ----------
discover_apps() {
    local d
    for d in "$TIER_DIR"/*/; do
        [[ -f "${d}main.c" ]] || continue
        basename "$d"
    done | sort
}

ALL_APPS="$(discover_apps)"

if [[ -n "$APPS_FILTER" ]]; then
    SELECTED=""
    for want in $APPS_FILTER; do
        found=0
        for a in $ALL_APPS; do
            if [[ "$a" == "$want" ]]; then
                SELECTED+="$a "
                found=1
                break
            fi
        done
        if [[ "$found" -eq 0 ]]; then
            echo -e "${YELLOW}Warning:${NC} requested app '$want' not under $TIER_DIR" >&2
        fi
    done
    APPS="$SELECTED"
else
    APPS="$ALL_APPS"
fi

if [[ -z "${APPS// }" ]]; then
    echo -e "${RED}Error:${NC} no apps to test" >&2
    exit 2
fi

# ---------- per-app harness ----------
# halt_and_read_pc <elf> <log>
# Echoes "<pc_hex> <lr_hex>" on stdout. Empty PC if the JLink session failed.
halt_and_read_pc() {
    local elf="$1"
    local log="$2"
    local jscript pc lr

    if [[ "$DRY_RUN" -eq 1 ]]; then
        # Mock: pretend the chip parked in ra_delay_ms.
        echo "0x02000B2E 0x02000245"
        return 0
    fi

    jscript="$(mktemp)"
    # Device CORTEX-M85 + -nogui 1 + -SelectEmuBySN avoid every JLink dialog
    # (chip P/N R7KA8D2KFLCAC is not in JLink's device DB as of v9.38a).
    cat > "$jscript" <<EOF
device CORTEX-M85
si 1
speed 4000
connect
halt
regs
q
EOF
    JLinkExe -nogui 1 -SelectEmuBySN 1086567198 -commanderscript "$jscript" >> "$log" 2>&1 || true
    rm -f "$jscript"

    # Parse PC + LR out of the JLink `regs` dump. JLink prints e.g.
    #   PC = 02000B2E, ...
    #   ...  LR = 02000245
    pc="$(grep -Eo 'PC[[:space:]]*=[[:space:]]*[0-9A-Fa-f]+' "$log" | tail -1 | awk -F= '{gsub(/ /,"",$2); print "0x"$2}')"
    lr="$(grep -Eo 'LR[[:space:]]*=[[:space:]]*[0-9A-Fa-f]+' "$log" | tail -1 | awk -F= '{gsub(/ /,"",$2); print "0x"$2}')"
    echo "${pc:-} ${lr:-}"
}

# classify <pc_hex> <addr2line_text>
# Echoes one of: PASS, WIP, FAIL, UNKNOWN
classify() {
    local pc="$1"
    local sym="$2"

    # Cortex-M lockup state.
    if [[ "${pc,,}" == "0xeffffffe" ]]; then
        echo "FAIL"
        return
    fi

    case "$sym" in
        *Default_Handler*|*HardFault_Handler*|*MemManage_Handler*|*BusFault_Handler*|*UsageFault_Handler*|*SecureFault_Handler*)
            echo "FAIL"
            return
            ;;
        *panic_halt*|*demo_panic_halt*|*usb_hid_panic_halt*|*usb_msc_panic_halt*|*internal_ra_fatal_error*)
            echo "WIP"
            return
            ;;
        *tx_thread_schedule*|*ra_time.c*|*ra_delay_ms*|*main.c*|*internal_*)
            echo "PASS"
            return
            ;;
    esac

    if [[ -z "$pc" ]]; then
        echo "UNKNOWN"
    else
        # Default: assume in user code if we got a PC but couldn't pattern-match.
        echo "UNKNOWN"
    fi
}

# ---------- aggregate run ----------
{
    echo "# EK-RA8D2 hardware smoke-test results"
    echo
    echo "Generated: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "Probe: on-board J-Link OB (R7KA8D2KF_CPU0)"
    echo "Settle window: ${SETTLE_S}s"
    if [[ "$DRY_RUN" -eq 1 ]]; then
        echo
        echo "**DRY RUN** -- JLink calls were skipped; PC values are mocked."
    fi
    echo
    echo "| App | Result | PC | Symbol |"
    echo "|---|---|---|---|"
} > "$RESULTS_MD"

FAILED=0
WIP_COUNT=0
NOBUILD=0
PASS=0
TOTAL=0

for app in $APPS; do
    TOTAL=$((TOTAL+1))
    elf="$TIER_DIR/$app/build/$app.elf"
    hex="$TIER_DIR/$app/build/$app.hex"
    log="$OUT_DIR/$app.log"
    : > "$log"

    echo -e "${BLUE}=== $app ===${NC}"

    if [[ ! -f "$elf" || ! -f "$hex" ]]; then
        echo "NOBUILD: missing $elf or $hex" | tee -a "$log"
        echo "| $app | NOBUILD | -- | (run \`make $app\`) |" >> "$RESULTS_MD"
        NOBUILD=$((NOBUILD+1))
        continue
    fi

    if [[ "$DRY_RUN" -eq 0 ]]; then
        echo "[flash] $hex" >> "$log"
        if ! bash "$ROOT_DIR/scripts/flash.sh" "$hex" >> "$log" 2>&1; then
            echo "| $app | FAIL | -- | flash error (see $app.log) |" >> "$RESULTS_MD"
            FAILED=$((FAILED+1))
            echo -e "${RED}FAIL${NC} (flash)"
            continue
        fi
        echo "[settle] sleeping ${SETTLE_S}s" >> "$log"
        sleep "$SETTLE_S"
    fi

    read -r pc lr < <(halt_and_read_pc "$elf" "$log")
    echo "[halt] PC=$pc LR=$lr" >> "$log"

    if [[ -z "$pc" ]]; then
        echo "| $app | FAIL | -- | JLink halt failed |" >> "$RESULTS_MD"
        FAILED=$((FAILED+1))
        echo -e "${RED}FAIL${NC} (no PC)"
        continue
    fi

    sym="$(arm-none-eabi-addr2line -f -e "$elf" "$pc" 2>>"$log" | tr '\n' ' ' | sed 's/ *$//')"
    echo "[addr2line] $sym" >> "$log"

    result="$(classify "$pc" "$sym")"
    # Escape pipe chars in symbol for Markdown table safety.
    sym_md="${sym//|/\\|}"
    echo "| $app | $result | $pc | ${sym_md:-(unresolved)} |" >> "$RESULTS_MD"

    case "$result" in
        PASS)    PASS=$((PASS+1));    echo -e "${GREEN}PASS${NC} $pc $sym" ;;
        WIP)     WIP_COUNT=$((WIP_COUNT+1)); echo -e "${YELLOW}WIP${NC}  $pc $sym" ;;
        FAIL)    FAILED=$((FAILED+1)); echo -e "${RED}FAIL${NC} $pc $sym" ;;
        UNKNOWN) WIP_COUNT=$((WIP_COUNT+1)); echo -e "${YELLOW}UNKNOWN${NC} $pc $sym" ;;
    esac
done

{
    echo
    echo "## Summary"
    echo
    echo "- Total apps: $TOTAL"
    echo "- PASS:    $PASS"
    echo "- WIP:     $WIP_COUNT"
    echo "- FAIL:    $FAILED"
    echo "- NOBUILD: $NOBUILD"
} >> "$RESULTS_MD"

echo
echo -e "${BLUE}=== Summary ===${NC}"
cat "$RESULTS_MD"
echo
echo "Results written to $RESULTS_MD"

if [[ "$FAILED" -gt 0 ]]; then
    exit 1
fi
exit 0
