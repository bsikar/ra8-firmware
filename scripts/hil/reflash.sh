#!/usr/bin/env bash
#
# hil_reflash.sh -- recover + reflash a TrustZone/RoT-provisioned EK-RA8D2.
#
# A board that was flashed with a TrustZone image or a Root-of-Trust bootloader
# (RA8_ENABLE_ROOT_OF_TRUST -- e.g. examples/.../dfu_bootloader with the RoT
# launch gate on) may not re-flash cleanly with `make hil-flash` alone:
#
#   1. The TrustZone boundary (SAU/IDAU option bytes) can gate the debug AP, so
#      J-Link cannot attach to reprogram code-MRAM.
#   2. The RoT anti-rollback counter (extra-MRAM @0x27000000) persists across a
#      normal flash, so a lower-versioned image is refused at boot.
#
# This does the full reset first -- `rfp-cli -erase-chip` (the boot-firmware
# Initialize command: clears the TrustZone boundary, the anti-rollback counter,
# and every code/data block back to OEM default; see scripts/hil/dlm_reset.sh
# for the DLM details) -- then a clean flash of <app>. Run it whenever a board
# that held a TrustZone or RoT image will not take an ordinary `make hil-flash`.
#
# Usage:
#   bash scripts/hil/reflash.sh <app>
#   make hil-reflash APP=<app>
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT

set -euo pipefail

APP="${1:?usage: hil_reflash.sh <app>}"
here="$(cd "$(dirname "$0")" && pwd)"

echo "[hil_reflash] full TrustZone/RoT reset via rfp-cli -erase-chip ..."
bash "${here}/hil_dlm_reset.sh"

echo "[hil_reflash] reset complete -- flashing ${APP} ..."
bash "${here}/hil_flash.sh" "${APP}"

echo "[hil_reflash DONE] ${APP} flashed to a freshly-reset board"
