#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# flash.sh -- flash the built ESP32-C6 co-processor firmware to the C6.
#
# Proven over the CH343 USB-UART bridge (VID:PID 1a86:55d3), which enumerates as
# /dev/ttyACM1 on the bench host. Do NOT use the native USB-JTAG interface for
# writing: it fails with EPIPE partway through write_flash.
#
# Usage:
#   ./c6_firmware/flash.sh                 # uses C6_FLASH_PORT from pins.env
#   ./c6_firmware/flash.sh /dev/ttyACM1    # explicit port
#
# Requires: esptool (python -m esptool) on PATH and a completed ./build.sh.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# shellcheck source=c6_firmware/pins.env
source "${SCRIPT_DIR}/pins.env"

PORT="${1:-${C6_FLASH_PORT}}"
BUILD_DIR="${SCRIPT_DIR}/esp-hosted-mcu/slave/build"

if ! command -v python >/dev/null 2>&1; then
  echo "ERROR: python not on PATH (need python -m esptool)" >&2
  exit 1
fi

if [[ ! -d "${BUILD_DIR}" ]]; then
  echo "ERROR: ${BUILD_DIR} not found. Run ./c6_firmware/build.sh first." >&2
  exit 1
fi

echo "==> flashing ESP32-C6 on ${PORT} (${C6_FLASH_SIZE}, ${C6_FLASH_MODE} @ ${C6_FLASH_FREQ})"
python -m esptool \
  --chip "${ESP_TARGET}" \
  -p "${PORT}" \
  -b "${C6_FLASH_BAUD}" \
  --before default_reset \
  --after hard_reset \
  --connect-attempts 5 \
  write_flash \
  --flash_mode "${C6_FLASH_MODE}" \
  --flash_size "${C6_FLASH_SIZE}" \
  --flash_freq "${C6_FLASH_FREQ}" \
  0x0 "${BUILD_DIR}/bootloader/bootloader.bin" \
  0x8000 "${BUILD_DIR}/partition_table/partition-table.bin" \
  0xd000 "${BUILD_DIR}/ota_data_initial.bin" \
  0x10000 "${BUILD_DIR}/network_adapter.bin"

echo "flash.sh: OK -- C6 flashed and hard-reset"
