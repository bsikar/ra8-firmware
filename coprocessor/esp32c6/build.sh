#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# build.sh -- reproducibly build the ESP32-C6 wireless co-processor firmware.
#
# The C6 runs Espressif's esp-hosted-mcu "network_adapter" application as SOUP:
# ZERO first-party code runs on the C6. This script fetches the pinned upstream
# at ESP_HOSTED_MCU_COMMIT, drops in the proven sdkconfig.defaults, and builds
# the peripheral-side app with a pinned esp-idf. See docs/SOUP/esp-hosted.md and
# docs/design/c6_wireless_architecture.md.
#
# Usage:
#   ./coprocessor/esp32c6/build.sh
#
# Requires (NONE of which live on the dev box -- build on the Pi bench host):
#   - esp-idf ESP_IDF_VERSION exported so idf.py is on PATH (. $IDF_PATH/export.sh)
#   - network access for git clone + the esp-idf component manager
#
# Verified: this recipe built, flashed, and booted on the bench with our pins.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# shellcheck source=coprocessor/esp32c6/pins.env
source "${SCRIPT_DIR}/pins.env"

CLONE_DIR="${SCRIPT_DIR}/esp-hosted-mcu"
PERIPHERAL_DIR="${CLONE_DIR}/slave" # LEGACY-OK: esp-hosted-mcu upstream directory is named "slave"

# ---- 1. require idf.py and assert the pinned esp-idf major.minor ----
if ! command -v idf.py >/dev/null 2>&1; then
  echo "ERROR: idf.py not on PATH. Export esp-idf ${ESP_IDF_VERSION} first" >&2
  echo "       (get_idf, or . \"\${IDF_PATH}/export.sh\"). The dev box has no" >&2
  echo "       esp-idf -- build on the Pi bench host." >&2
  exit 1
fi

idf_version="$(idf.py --version 2>/dev/null || true)"
case "${idf_version}" in
  *v5.5.*) : ;;
  *)
    echo "ERROR: esp-idf ${ESP_IDF_VERSION} required; idf.py reports: ${idf_version:-unknown}" >&2
    exit 1
    ;;
esac
echo "==> idf.py: ${idf_version}"

# ---- 2. fetch esp-hosted-mcu at the pinned commit ----
if [[ ! -d "${CLONE_DIR}/.git" ]]; then
  echo "==> cloning ${ESP_HOSTED_MCU_URL}"
  git clone "${ESP_HOSTED_MCU_URL}" "${CLONE_DIR}"
fi
echo "==> checking out ${ESP_HOSTED_MCU_SHORT} (${ESP_HOSTED_MCU_COMMIT})"
git -C "${CLONE_DIR}" fetch origin
git -C "${CLONE_DIR}" -c advice.detachedHead=false checkout --detach "${ESP_HOSTED_MCU_COMMIT}"

if [[ ! -d "${PERIPHERAL_DIR}" ]]; then
  echo "ERROR: ${PERIPHERAL_DIR} missing -- upstream layout changed at this commit" >&2
  exit 1
fi

# ---- 3. drop in the proven sdkconfig.defaults ----
echo "==> installing sdkconfig.defaults"
cp "${SCRIPT_DIR}/sdkconfig.defaults" "${PERIPHERAL_DIR}/sdkconfig.defaults"

# ---- 4. clean, set target, build ----
echo "==> cleaning previous build state"
rm -rf \
  "${PERIPHERAL_DIR}/build" \
  "${PERIPHERAL_DIR}/sdkconfig" \
  "${PERIPHERAL_DIR}/dependencies.lock" \
  "${PERIPHERAL_DIR}/managed_components"

echo "==> idf.py set-target ${ESP_TARGET} && idf.py build"
(
  cd "${PERIPHERAL_DIR}"
  idf.py set-target "${ESP_TARGET}"
  idf.py build
)

# ---- 5. print artifact paths ----
echo "build.sh: OK -- flash these with ./coprocessor/esp32c6/flash.sh:"
for artifact in \
  "bootloader/bootloader.bin" \
  "partition_table/partition-table.bin" \
  "ota_data_initial.bin" \
  "network_adapter.bin"; do
  echo "  ${PERIPHERAL_DIR}/build/${artifact}"
done
