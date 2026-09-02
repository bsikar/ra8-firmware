#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
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
#   /bin/bash -p coprocessor/esp32c6/flash.sh           # resolve the CH343 bridge
#   /bin/bash -p coprocessor/esp32c6/flash.sh <device>  # explicit port
#
# Requires: esptool (python -m esptool) on PATH and a completed ./build.sh.

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
    echo "ERROR: ${BUILD_DIR} not found. Run /bin/bash -p coprocessor/esp32c6/build.sh first." >&2
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
else
  [[ "$-" == *p* ]]
fi
