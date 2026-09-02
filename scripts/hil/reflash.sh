#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# hil_reflash.sh -- recover + reflash a TrustZone/RoT-provisioned EK-RA8D2.
#
# A board that was flashed with a TrustZone image or a Root-of-Trust bootloader
# (RA8_ENABLE_ROOT_OF_TRUST -- e.g. examples/.../dfu_bootloader with the RoT
# launch gate on) may not re-flash cleanly with `just hil::flash` alone:
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
# that held a TrustZone or RoT image will not take an ordinary `just hil::flash`.
#
# Usage:
#   /bin/bash -p scripts/hil/reflash.sh <app>
#   just hil::reflash <app>
#

if [[ "$-" == *p* ]]; then
  unset -v BASH_ENV ENV
  declare -a ra8_startup_env_unset=()
  _ra8_startup_refuse() {
    printf 'error: privileged startup %s\n' "$1" >&2
    exit 1
  }
  ra8_startup_env_done_count=0
  while IFS= read -r -d '' ra8_startup_env_row; do
    ra8_startup_env_name="${ra8_startup_env_row%%=*}"
    case "$ra8_startup_env_name" in
      RA8_STARTUP_ENV_DONE)
        ra8_startup_env_done_count=$((ra8_startup_env_done_count + 1))
        ;;
      BASH_FUNC_*%% | BASH_FUNC_*'()') ra8_startup_env_unset+=(-u "$ra8_startup_env_name") ;;
    esac
  done < <(
    /usr/bin/env -u RA8_STARTUP_ENV_DONE -0 &&
      /usr/bin/printf 'RA8_STARTUP_ENV_DONE=1\0'
  )
  ((ra8_startup_env_done_count == 1)) && [[ "$ra8_startup_env_name" == RA8_STARTUP_ENV_DONE ]] || _ra8_startup_refuse 'environment enumeration was incomplete'
  if ((${#ra8_startup_env_unset[@]})); then
    [[ -z "${RA8_STARTUP_ENV_SCRUBBED-}" ]] || _ra8_startup_refuse 'scrub did not converge'
    ra8_startup_reentry="$0"
    [[ "$ra8_startup_reentry" == */* ]] || _ra8_startup_refuse 'requires a script path'
    if [[ "$ra8_startup_reentry" != /* ]]; then
      ra8_startup_reentry="$PWD/$ra8_startup_reentry"
    fi
    ra8_startup_check="$ra8_startup_reentry"
    while [[ "$ra8_startup_check" != "/" ]]; do
      [[ ! -L "$ra8_startup_check" ]] || _ra8_startup_refuse 'refuses a symlinked path'
      ra8_startup_parent="${ra8_startup_check%/*}"
      [[ -n "$ra8_startup_parent" ]] || ra8_startup_parent="/"
      [[ "$ra8_startup_parent" != "$ra8_startup_check" ]] ||
        _ra8_startup_refuse 'cannot validate its script path'
      ra8_startup_check="$ra8_startup_parent"
    done
    [[ -f "$ra8_startup_reentry" ]] || _ra8_startup_refuse 'refuses a non-regular path'
    if ! exec /usr/bin/env "${ra8_startup_env_unset[@]}" -u BASH_ENV -u ENV \
      -u RA8_STARTUP_ENV_DONE RA8_STARTUP_ENV_SCRUBBED=1 \
      /bin/bash -p -- "$ra8_startup_reentry" "$@"; then
      _ra8_startup_refuse 'could not enter sanitized process'
    fi
  fi
  unset -v ra8_startup_check ra8_startup_env_done_count
  unset -v ra8_startup_env_name ra8_startup_env_row
  unset -v ra8_startup_env_unset ra8_startup_parent ra8_startup_reentry
  unset -v RA8_STARTUP_ENV_DONE
  unset -v RA8_STARTUP_ENV_SCRUBBED
  unset -f _ra8_startup_refuse

  set -euo pipefail

  APP_ID="${1:?usage: hil_reflash.sh <app>}"
  here="$(cd "$(dirname "$0")" && pwd)"

  # ---- anti-recovery pre-flash guard (BEFORE the destructive erase) ------------
  # Refuse a bricking image up front, so the board is not erased for an image the
  # flash step would refuse anyway. See scripts/hil/lib/preflash_guard.sh.
  _root="$(cd "${here}/../.." && pwd)"
  _app_dir="$(python3 "${_root}/scripts/dev/ra8_apps.py" dir "${APP_ID}" 2>/dev/null || true)"
  APP="$(python3 "${_root}/scripts/dev/ra8_apps.py" name "${APP_ID}" 2>/dev/null || true)"
  if [[ -z "${_app_dir}" || -z "${APP}" ]]; then
    echo "[hil_reflash] app '${APP_ID}' not found under examples/" >&2
    exit 1
  fi
  _hex="${_app_dir}/build/${APP}.hex"
  [[ -f "${_hex}" ]] ||
    /bin/bash -p "${_root}/scripts/dev/run_just.sh" --justfile "${_root}/justfile" apps::build "${APP_ID}"
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
  ra8_bench_require_recovery "TrustZone/RoT reset then reflash ${APP_ID}" 30m || exit $?

  echo "[hil_reflash] full TrustZone/RoT reset via rfp-cli -erase-chip ..."
  # These two were `hil_dlm_reset.sh` and `hil_flash.sh` -- names that stopped
  # existing when the scripts were renamed, so the reflash entry point had been dead
  # for the whole life of the rename with nothing to notice: the paths are built
  # from a variable, which is exactly the shape check_script_references.py cannot
  # resolve.
  /bin/bash -p "${here}/dlm_reset.sh"

  echo "[hil_reflash] reset complete -- flashing ${APP_ID} ..."
  /bin/bash -p "${here}/flash.sh" "${APP_ID}"

  echo "[hil_reflash DONE] ${APP_ID} flashed to a freshly-reset board"
else
  [[ "$-" == *p* ]]
fi
