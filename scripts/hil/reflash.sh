#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
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

set -euo pipefail

APP="${1:?usage: hil_reflash.sh <app>}"
here="$(cd "$(dirname "$0")" && pwd)"

# ---- anti-recovery pre-flash guard (BEFORE the destructive erase) ------------
# Refuse a bricking image up front, so the board is not erased for an image the
# flash step would refuse anyway. See scripts/hil/lib/preflash_guard.sh.
_root="$(cd "${here}/../.." && pwd)"
_app_dir="$(find "${_root}/examples" -name main.c -print0 |
  xargs -0 -n1 dirname |
  { while read -r _d; do [[ "$(basename "$_d")" == "${APP}" ]] && echo "$_d"; done; } |
  head -1)"
if [[ -z "${_app_dir}" ]]; then
  echo "[hil_reflash] app '${APP}' not found under examples/" >&2
  exit 1
fi
_hex="${_app_dir}/build/${APP}.hex"
[[ -f "${_hex}" ]] || make -C "${_root}" "${APP}"
# shellcheck source=scripts/hil/lib/preflash_guard.sh
source "${here}/lib/preflash_guard.sh"
ra8_preflash_guard "${_hex}" || exit $?

# ---- bench mutual exclusion --------------------------------------------------
# Taken HERE, once, and inherited by the two scripts below through
# RA8_BENCH_LOCK_ID -- so the erase and the reflash are one indivisible
# operation from the bench's point of view. Acquiring twice would let a third
# actor in between them and flash over a freshly-erased chip.
# shellcheck source=scripts/hil/lib/bench_lock.sh
source "${here}/lib/bench_lock.sh"
ra8_bench_require_recovery "TrustZone/RoT reset then reflash ${APP}" 30m || exit $?

echo "[hil_reflash] full TrustZone/RoT reset via rfp-cli -erase-chip ..."
# These two were `hil_dlm_reset.sh` and `hil_flash.sh` -- names that stopped
# existing when the scripts were renamed, so `make hil-reflash` had been dead
# for the whole life of the rename with nothing to notice: the paths are built
# from a variable, which is exactly the shape check_script_references.py cannot
# resolve.
bash "${here}/dlm_reset.sh"

echo "[hil_reflash] reset complete -- flashing ${APP} ..."
bash "${here}/flash.sh" "${APP}"

echo "[hil_reflash DONE] ${APP} flashed to a freshly-reset board"
