#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# flash.sh -- flash the built ESP32-C6 co-processor firmware to the C6.
#
# Proven over the CH343 USB-UART bridge (VID:PID 1a86:55d3). Do NOT use the
# native USB-JTAG interface for writing: it fails with EPIPE partway through
# write_flash. The two are easy to confuse -- both enumerate as /dev/ttyACM<n>
# and the numbering changes on a power cycle -- so the bridge is resolved by
# device identity through scripts/hil/lib/tty_resolve.sh rather than named by
# number.
#
# Usage:
#   ./coprocessor/esp32c6/flash.sh                 # resolve the CH343 bridge
#   ./coprocessor/esp32c6/flash.sh <device>        # explicit port
#
# Requires: esptool (python -m esptool) on PATH and a completed ./build.sh.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# shellcheck source=coprocessor/esp32c6/pins.env
source "${SCRIPT_DIR}/pins.env"
# shellcheck source=scripts/hil/lib/tty_resolve.sh
source "${REPO_ROOT}/scripts/hil/lib/tty_resolve.sh"

PORT="${1:-${C6_FLASH_PORT}}"
if [[ -z "${PORT}" ]]; then
  PORT="$(ra8_tty_resolve c6)" || exit 1
fi
BUILD_DIR="${SCRIPT_DIR}/esp-hosted-mcu/slave/build" # LEGACY-OK: esp-hosted-mcu upstream layout dir is named "slave"

if ! command -v python >/dev/null 2>&1; then
  echo "ERROR: python not on PATH (need python -m esptool)" >&2
  exit 1
fi

if [[ ! -d "${BUILD_DIR}" ]]; then
  echo "ERROR: ${BUILD_DIR} not found. Run ./coprocessor/esp32c6/build.sh first." >&2
  exit 1
fi

# ---- bench mutual exclusion --------------------------------------------------
# The C6 is not a separate resource. It is soldered to Pmod1 (J26): powered by
# the board, reset by power-cycling it, and sharing the SCI2 Simple-SPI bus and
# the SW4-3 mux. Flashing it while somebody flashes the RA8 collides on the same
# assembly, and esptool's `--after hard_reset` toggles lines the RA8 side is
# reading. So it takes the same lock.
# shellcheck source=scripts/hil/lib/bench_lock.sh
source "${REPO_ROOT}/scripts/hil/lib/bench_lock.sh"
ra8_bench_require "flash the ESP32-C6 co-processor on ${PORT}" || exit $?

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
