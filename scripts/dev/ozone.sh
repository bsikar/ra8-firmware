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
