#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# hil_erase.sh -- Mass-erase the EK-RA8D2 MRAM via J-Link over SSH to the Pi.
#
# Use this when the board is in a state where hil_flash.sh fails with
# "Failed to configure AP" or "CPU could not be halted".  Those errors
# mean the currently running firmware is holding the secure domain and
# blocking debug halt.  A mass erase wipes the firmware, leaving the
# board blank so the next hil_flash.sh invocation connects cleanly.
#
# Usage:
#   bash scripts/hil/erase.sh
#
# Exit codes:
#   0  -- erase confirmed
#   1  -- erase failed
#   2  -- Pi unreachable

set -euo pipefail

# Rig config (PI_HOST, JLINK_SN) comes from the gitignored .env, not the tree.
_hil_dir="$(dirname "${BASH_SOURCE[0]}")"
_hil_dir="$(cd "$_hil_dir" && pwd)"
# shellcheck source=scripts/hil/lib/rig_env.sh
source "$_hil_dir/lib/rig_env.sh"
rig_require PI_HOST JLINK_SN

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

tag() { printf "${YELLOW}[hil_erase]${NC} %s\n" "$*"; }
ok() { printf "${GREEN}[OK]${NC}  %s\n" "$*"; }
err() { printf "${RED}[FAIL]${NC} %s\n" "$*"; }

tag "checking Pi ${PI_HOST}..."
ssh -o ConnectTimeout=5 -o BatchMode=yes "$PI_HOST" true 2>/dev/null ||
  {
    err "cannot reach ${PI_HOST}"
    exit 2
  }
ok "Pi reachable"

tag "erasing MRAM (this takes ~5s)..."

# shellcheck disable=SC2087  # local vars (JLINK_DEVICE, JLINK_SN) expand client-side; remote vars are escaped
ssh "$PI_HOST" bash <<REMOTE
set -euo pipefail
TMP=\$(mktemp)
LOG=/tmp/hil_erase.log
trap 'rm -f "\$TMP"' EXIT

cat > "\$TMP" <<JLINK
device ${JLINK_DEVICE}
si SWD
speed 4000
connect
erase
q
JLINK

JLinkExe -nogui 1 -SelectEmuBySN ${JLINK_SN} -commanderscript "\$TMP" > "\$LOG" 2>&1 || true

echo "---- erase log ----"
grep -iE "erase|O\.K\.|error|warning|VTref|Cortex|complete|done|failed" "\$LOG" || cat "\$LOG"
echo "-------------------"

if grep -qiE "Erase done|Erasing done|O\.K\." "\$LOG"; then
    echo "ERASE_OK"
elif grep -q "Cortex-M85 identified" "\$LOG"; then
    echo "CONNECTED_NO_CONFIRM"
else
    echo "ERASE_FAIL"
fi
REMOTE

# Re-run just to get the status token from the heredoc output
STATUS=$(
  ssh "$PI_HOST" bash <<REMOTE2
LOG=/tmp/hil_erase.log
if grep -qiE "Erase done|Erasing done" "\$LOG" 2>/dev/null; then
    echo "OK"
elif grep -q "Cortex-M85 identified" "\$LOG" 2>/dev/null; then
    echo "PARTIAL"
else
    echo "FAIL"
fi
REMOTE2
)

case "$STATUS" in
  OK)
    ok "erase complete -- board is blank, safe to flash now"
    exit 0
    ;;
  PARTIAL)
    tag "connected but no erase confirmation -- trying to flash anyway may still work"
    tag "check full log on Pi: /tmp/hil_erase.log"
    exit 1
    ;;
  *)
    err "erase failed -- check /tmp/hil_erase.log on Pi"
    exit 1
    ;;
esac
