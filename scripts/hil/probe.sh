#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# hil_probe.sh -- Quick J-Link + board diagnostic over SSH to the Pi.
# Reports: Pi reachability, J-Link USB device presence, VTref, SWD
# connect, core halt state.  No firmware is written.
#
# Usage:
#   bash scripts/hil/probe.sh
#
# Exit codes:
#   0  -- probe fully connected and core halted OK
#   1  -- partial failure (see output)
#   2  -- Pi unreachable

set -euo pipefail

# Rig config (PI_HOST, JLINK_SN) comes from the gitignored .env, not the tree.
_hil_dir="$(dirname "${BASH_SOURCE[0]}")"
_hil_dir="$(cd "$_hil_dir" && pwd)"
# shellcheck source=scripts/hil/lib/rig_env.sh
source "$_hil_dir/lib/rig_env.sh"
rig_require PI_HOST JLINK_SN

# ---- bench mutual exclusion --------------------------------------------------
# One actor at a time on the physical bench. The hold lives exactly as long as
# this script does -- it is a live process on a kernel flock, not a lease -- so
# nothing here can leave the bench stale. See scripts/hil/bench.sh.
# shellcheck source=scripts/hil/lib/bench_lock.sh
source "$_hil_dir/lib/bench_lock.sh"
ra8_bench_require "J-Link + board diagnostic" || exit $?

GREEN='\033[0;32m'
RED='\033[0;31m'
CYAN='\033[0;36m'
NC='\033[0m'

tag() { printf "${CYAN}[hil_probe]${NC} %s\n" "$*"; }
ok() { printf "${GREEN}[OK]${NC}  %s\n" "$*"; }
err() { printf "${RED}[FAIL]${NC} %s\n" "$*"; }

# ---- 1. Pi reachable? --------------------------------------------------------
tag "checking Pi ${PI_HOST}..."
if ! ssh -o ConnectTimeout=5 -o BatchMode=yes "$PI_HOST" true 2>/dev/null; then
  err "cannot reach ${PI_HOST}"
  exit 2
fi
ok "Pi reachable"

# ---- 2. J-Link USB device visible on Pi? -------------------------------------
tag "checking USB devices on Pi..."
USB_INFO=$(ssh "$PI_HOST" "lsusb 2>/dev/null | grep -i 'segger\|j-link\|1366:' || echo 'NOT_FOUND'")
echo "    lsusb : ${USB_INFO}"
if [[ "$USB_INFO" == "NOT_FOUND" ]]; then
  err "J-Link USB device not found on Pi -- recheck USB cable or replug the board"
  exit 1
fi
ok "J-Link USB device visible"

# ---- 3. JLinkExe present on Pi? ----------------------------------------------
tag "checking JLinkExe on Pi..."
if ! ssh "$PI_HOST" "command -v JLinkExe >/dev/null 2>&1"; then
  err "JLinkExe not found in PATH on ${PI_HOST}"
  exit 1
fi
ok "JLinkExe found"

# ---- 4. Run probe script on Pi (all parsing done remotely) -------------------
tag "connecting to J-Link SN=${JLINK_SN}, device=${JLINK_DEVICE}..."

# shellcheck disable=SC2087  # client-side substitution of JLINK_SN/JLINK_DEVICE is intentional
ssh "$PI_HOST" bash <<REMOTE
set -euo pipefail
TMP=\$(mktemp)
LOG=\$(mktemp)
trap 'rm -f "\$TMP" "\$LOG"' EXIT

cat > "\$TMP" <<JLINK
device ${JLINK_DEVICE}
si SWD
speed 4000
connect
ShowHWStatus
q
JLINK

JLinkExe -nogui 1 -SelectEmuBySN ${JLINK_SN} -commanderscript "\$TMP" > "\$LOG" 2>&1 || true

echo ""
echo "---- J-Link output ----"
grep -iE "O\.K\.|FAILED|Error|Warning|VTref|Cortex|DAP|AP\[|could not|halt|SWD|S/N:|uptime|Firmware" "\$LOG" || cat "\$LOG"
echo "-----------------------"
echo ""

VTREF=\$(grep -oE 'VTref=[0-9.]+V' "\$LOG" | head -1 | sed 's/VTref=//' || echo "unknown")
SN_OK=\$(grep -c "S/N: ${JLINK_SN}" "\$LOG" || true)
CONNECT_OK=\$(grep -c "Cortex-M85 identified" "\$LOG" || true)
HALT_WARN=\$(grep -c "CPU could not be halted" "\$LOG" || true)
DAP_FAIL=\$(grep -c "Failed to power up DAP" "\$LOG" || true)
AP_FAIL=\$(grep -c "Failed to configure AP" "\$LOG" || true)
USB_FAIL=\$(grep -c "Cannot connect to the probe" "\$LOG" || true)

echo "--- summary ---"
[[ "\$SN_OK"      -gt 0 ]] && echo "OK   J-Link probe found (SN ${JLINK_SN})" \
                           || echo "FAIL J-Link probe NOT found -- USB issue?"
[[ "\$USB_FAIL"   -gt 0 ]] && echo "FAIL Cannot connect to probe -- replug USB or reboot Pi"
echo "     VTref : \${VTREF}"
[[ "\$VTREF" != "0.000V" && "\$VTREF" != "unknown" ]] \
    && echo "OK   VTref=\${VTREF} (board powered)" \
    || echo "FAIL VTref=\${VTREF} -- board not powered?"
[[ "\$CONNECT_OK" -gt 0 ]] && echo "OK   Cortex-M85 identified" \
                           || echo "FAIL could not identify Cortex-M85"
[[ "\$DAP_FAIL" -gt 0 && "\$CONNECT_OK" -eq 0 ]] && echo "FAIL DAP power-up failed -- TrustZone/SSD or board state" || \
[[ "\$DAP_FAIL" -gt 0 && "\$CONNECT_OK" -gt 0 ]] && echo "WARN DAP first-attempt retry (normal for RA8D2) -- recovered OK" || true
[[ "\$AP_FAIL"    -gt 0 ]] && echo "FAIL AP configuration failed -- try hil_erase.sh then reflash"
[[ "\$HALT_WARN"  -gt 0 ]] && echo "FAIL CPU could not be halted -- firmware blocking debug" \
                           || echo "OK   no halt warnings"

if [[ "\$CONNECT_OK" -gt 0 && "\$HALT_WARN" -eq 0 && "\$AP_FAIL" -eq 0 ]]; then
    echo ""
    echo "OK   probe healthy -- ready to flash"
    exit 0
else
    echo ""
    echo "FAIL probe issues detected -- see above"
    exit 1
fi
REMOTE
