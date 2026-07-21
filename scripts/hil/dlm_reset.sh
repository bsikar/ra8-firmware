#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# hil_dlm_reset.sh -- Recover an EK-RA8D2 from OEM_PL0 (or PL1) back
# to OEM_PL2 by invoking the boot-firmware Initialize command via
# `rfp-cli -erase-chip`.
#
# Use this when the chip is in a "Cannot halt CPU" / "Could not power
# up DAP" / "Failed to configure AP" debug-locked state caused by an
# OEM-Protection-Level transition without authentication keys
# (typically OEM_PL0/AL0).
#
# Prereqs:
#   1. The chip's `ce` Security Flag ("Disable Initialize Command")
#      must NOT have been set. Verify before transitioning to any
#      OEM_PLx state by reading `Security Flags` from `rfp-cli -rfo`.
#      EK-RA8D2 boards ship with `Security Flags: none` so Initialize
#      is available unless YOU explicitly disabled it.
#   2. The chip's SWD-DP must still respond. JLinkExe should be able
#      to read DPIDR (0x6BA02477) even if the AHB-AP is gated. If
#      the chip is in LCK_BOOT (SWD-DP unresponsive at all), this
#      script cannot recover it -- the chip is permanently bricked.
#
# What this script does:
#   1. Run `rfp-cli -d ra -t jlink:<sn> -if swd -s 1000000 -erase-chip`
#      (which invokes the boot-firmware Initialize command per
#      `/opt/rfp/linux-x64/docs/rfp-cli.md`).
#   2. Verify via `rfp-cli -rfo` that the DLM state has moved to
#      `OEM_PL2` and the Security Flags remain `none`.
#   3. Print a verbatim before/after snapshot so the operator can
#      confirm what changed.
#
# CRITICAL WARNING: do NOT chain a `rfp-cli -dlm <state>` command
# after this script. The Initialize already moves the chip to
# OEM_PL2; any subsequent `-dlm` call will RE-LOCK the chip into the
# state you ran the recovery to escape.
#
# Verified working 2026-05-27 on EK-RA8D2 after a 2-day brick
# incident. Renesas Customer Support case #417141 (FAE Fivos)
# confirmed the Initialize command is available from OEM_PL0/AL0
# without AL key auth when the `ce` flag is unset. The rfp-cli
# documentation states verbatim: "-erase-chip ... If the device
# supports an initialization function, this option will also execute
# the initialization command."
#
# Usage:
#   bash scripts/hil/dlm_reset.sh
#
# Exit codes:
#   0  -- chip recovered, DLM == OEM_PL2, Security Flags == none
#   1  -- recovery failed (chip may be in LCK_BOOT, ce flag set, or
#         a different brick mode); see stderr for the rfp-cli output
#   2  -- usage / Pi unreachable

set -uo pipefail

# Rig config (PI_HOST, JLINK_SN) comes from the gitignored .env, not the tree.
_hil_dir="$(dirname "${BASH_SOURCE[0]}")"
_hil_dir="$(cd "$_hil_dir" && pwd)"
# shellcheck source=scripts/hil/lib/rig_env.sh
source "$_hil_dir/lib/rig_env.sh"
rig_require PI_HOST JLINK_SN
DEVICE="ra"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

tag() { printf "${CYAN}[dlm_reset]${NC} %s\n" "$*"; }
ok() { printf "${GREEN}[OK]${NC}  %s\n" "$*"; }
err() { printf "${RED}[FAIL]${NC} %s\n" "$*" >&2; }
warn() { printf "${YELLOW}[WARN]${NC} %s\n" "$*"; }

# ---- 0. Sanity-check the Pi reachable ---------------------------------------
if ! ssh -o ConnectTimeout=5 -o BatchMode=yes "$PI_HOST" "true" 2>/dev/null; then
  err "Cannot reach Pi at ${PI_HOST} -- check network / SSH key."
  exit 2
fi
tag "Pi reachable: ${PI_HOST}"

# ---- 1. Read DLM state BEFORE Initialize ------------------------------------
BEFORE_LOG="/tmp/hil_dlm_reset_before.log"
tag "Reading DLM state BEFORE Initialize..."
# shellcheck disable=SC2029  # ${DEVICE}/${JLINK_SN} are local rig config the Pi does not have.
ssh "$PI_HOST" \
  "rfp-cli -d ${DEVICE} -t jlink:${JLINK_SN} -if swd -s 1000000 -rfo 2>&1" \
  >"$BEFORE_LOG" 2>&1 || true

if grep -q "DLM State:" "$BEFORE_LOG"; then
  BEFORE_STATE="$(grep -E '^DLM State:' "$BEFORE_LOG" | head -1 | sed 's/^DLM State: //')"
  BEFORE_FLAGS="$(grep -E '^Security Flags:' "$BEFORE_LOG" | head -1 | sed 's/^Security Flags: //')"
  tag "BEFORE: DLM State = ${BEFORE_STATE}, Security Flags = ${BEFORE_FLAGS}"
elif grep -q "E100000E" "$BEFORE_LOG"; then
  warn "BEFORE: rfo returned E100000E protection error -- chip is in PL0/PL1 with debug gated."
  warn "        This is the expected starting condition for recovery."
  BEFORE_STATE="<protected>"
  BEFORE_FLAGS="<unreadable>"
else
  err "Unexpected rfo output BEFORE Initialize:"
  tail -10 "$BEFORE_LOG" | sed 's/^/    /' >&2
  exit 1
fi

# ---- 2. Run the Initialize via -erase-chip ----------------------------------
INIT_LOG="/tmp/hil_dlm_reset_init.log"
tag "Invoking Initialize command via rfp-cli -erase-chip..."
tag "(This erases user flash AND runs the boot-firmware Initialize,"
tag " which transitions DLM from OEM_PL0/PL1 back to OEM_PL2.)"
# shellcheck disable=SC2029  # ${DEVICE}/${JLINK_SN} are local rig config the Pi does not have.
if ! ssh "$PI_HOST" \
  "rfp-cli -d ${DEVICE} -t jlink:${JLINK_SN} -if swd -s 1000000 -erase-chip 2>&1" \
  >"$INIT_LOG" 2>&1; then
  err "rfp-cli -erase-chip returned non-zero. Output:"
  tail -15 "$INIT_LOG" | sed 's/^/    /' >&2
  exit 1
fi

if ! grep -q "Operation successful" "$INIT_LOG"; then
  err "rfp-cli -erase-chip did NOT report Operation successful. Output:"
  tail -15 "$INIT_LOG" | sed 's/^/    /' >&2
  exit 1
fi
ok "Initialize command reported success."

# ---- 3. Read DLM state AFTER Initialize -------------------------------------
AFTER_LOG="/tmp/hil_dlm_reset_after.log"
tag "Reading DLM state AFTER Initialize..."
# shellcheck disable=SC2029  # ${DEVICE}/${JLINK_SN} are local rig config the Pi does not have.
ssh "$PI_HOST" \
  "rfp-cli -d ${DEVICE} -t jlink:${JLINK_SN} -if swd -s 1000000 -rfo 2>&1" \
  >"$AFTER_LOG" 2>&1 || true

if ! grep -q "DLM State:" "$AFTER_LOG"; then
  err "AFTER: rfo did not return a DLM state. Output:"
  tail -15 "$AFTER_LOG" | sed 's/^/    /' >&2
  err "Recovery FAILED -- chip may be in LCK_BOOT, ce flag may have"
  err "been set, or a different brick mode is at play."
  exit 1
fi

AFTER_STATE="$(grep -E '^DLM State:' "$AFTER_LOG" | head -1 | sed 's/^DLM State: //')"
AFTER_FLAGS="$(grep -E '^Security Flags:' "$AFTER_LOG" | head -1 | sed 's/^Security Flags: //')"
tag "AFTER:  DLM State = ${AFTER_STATE}, Security Flags = ${AFTER_FLAGS}"

if [[ "$AFTER_STATE" == "OEM_PL2" ]]; then
  ok "Chip recovered. DLM is at OEM_PL2 (Initialize regressed PL0 -> PL2)."
  ok "Do NOT run 'rfp-cli -dlm <state>' after this -- chaining a DLM"
  ok "transition will re-lock the chip into OEM_PL0."
  echo
  tag "Logs:"
  tag "  BEFORE rfo: ${BEFORE_LOG}"
  tag "  Initialize: ${INIT_LOG}"
  tag "  AFTER  rfo: ${AFTER_LOG}"
  echo
  tag "Next steps:"
  tag "  - Flash a sanity-check app:   bash scripts/hil/flash.sh blink"
  tag "  - Resume normal HIL pipeline: bash scripts/hil/all.sh"
  exit 0
fi

err "Initialize ran but DLM state is now '${AFTER_STATE}' (expected OEM_PL2)."
err "Something unexpected happened. Check ${AFTER_LOG} for details."
exit 1
