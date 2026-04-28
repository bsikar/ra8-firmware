#!/usr/bin/env bash
# ozone.sh -- launch SEGGER Ozone debugger with ra8d2-firmware preloaded.
#
# Usage:
#   ./scripts/ozone.sh        or       make ozone
#
# Requires:
#   - SEGGER Ozone installed (cask: segger-ozone)
#   - EK-RA8D2 plugged in via USB
#   - Firmware built (make build) -- ELF at build/ra8d2-firmware.elf

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FW_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
JDEBUG="${SCRIPT_DIR}/ra8d2.jdebug"
ELF="${FW_DIR}/build/ra8d2-firmware.elf"

OZONE_BIN=""
for candidate in \
  "/Applications/SEGGER/Ozone/Ozone.app/Contents/MacOS/Ozone" \
  "/Applications/Ozone.app/Contents/MacOS/Ozone" \
  "$(command -v Ozone 2>/dev/null || true)"
do
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
  echo "  build: make build" >&2
  exit 1
fi

echo "==> Opening ${JDEBUG} in Ozone"
exec "${OZONE_BIN}" "${JDEBUG}"
