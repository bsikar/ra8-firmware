#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# ozone.sh -- launch SEGGER Ozone debugger with a per-app ELF preloaded.
#
# Usage:
#   ./scripts/dev/ozone.sh path/to/firmware.elf       # debug a specific elf (preferred)
#   ./scripts/dev/ozone.sh                            # default: examples/ek_ra8d2/blink/build/blink.elf
#
# Per-app Makefiles invoke this with their own .elf path
# (e.g. `make -C examples/blink_hal ozone` -> `ozone.sh examples/blink_hal/build/blink_hal.elf`).
#
# Requires:
#   - SEGGER Ozone installed (cask: segger-ozone)
#   - EK-RA8D2 plugged in via USB
#   - Firmware built (`make blink` etc.)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FW_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
JDEBUG="${SCRIPT_DIR}/ra8d2.jdebug"
ELF="${1:-${FW_DIR}/examples/ek_ra8d2/blink/build/blink.elf}"
export RA8_OZONE_ELF="${ELF}"

OZONE_BIN=""

# ---- bench mutual exclusion --------------------------------------------------
# There is ONE EK-RA8D2 in this project; "attached to this machine" is where it
# happens to be plugged in, not a second board. A debugger that halts the core
# is every bit as disruptive to somebody else's run as a flash, so it takes the
# same lock -- and holds it for the whole session, which is why the budget is
# hours rather than minutes.
# shellcheck source=scripts/hil/lib/bench_lock.sh
source "$SCRIPT_DIR/../hil/lib/bench_lock.sh"
ra8_bench_require "local SEGGER Ozone session" 2h || exit $?
for candidate in \
  "/Applications/SEGGER/Ozone/Ozone.app/Contents/MacOS/Ozone" \
  "/Applications/Ozone.app/Contents/MacOS/Ozone" \
  "$(command -v Ozone 2>/dev/null || true)"; do
  if [[ -n "${candidate}" && -x "${candidate}" ]]; then
    OZONE_BIN="${candidate}"
    break
  fi
done

if [[ -z "${OZONE_BIN}" ]]; then
  echo "error: Ozone not found." >&2
  echo "  install: brew install --cask segger-ozone" >&2
  exit 1
fi

if [[ ! -f "${ELF}" ]]; then
  echo "error: ${ELF} not found." >&2
  echo "  build: make <app>" >&2
  exit 1
fi

echo "==> Opening ${JDEBUG} in Ozone"
exec "${OZONE_BIN}" "${JDEBUG}"
