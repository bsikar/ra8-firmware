#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# hil_suite.sh -- Run the full HIL test suite for hw_validated/hil/ apps.
#
# Each entry in the TESTS table below maps an app name to the UART string
# that must appear on the board console within a timeout after flashing.
# Tests run sequentially (one board).  The script exits non-zero if any
# test fails and prints a summary at the end.
#
# Usage (run from the repo root on the Pi after `make <apps>` has built them):
#   bash scripts/hil/suite.sh [--uart <device>]
#
# The hex for each app is expected at:
#   examples/ek_ra8d2/hw_validated/hil/<app>/build/<app>.hex

set -euo pipefail

# Empty by default: run_direct.sh resolves the board console by device
# identity (scripts/hil/lib/tty_resolve.sh). --uart pins it when needed.
UART=""
ONLY=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --uart)
      UART="$2"
      shift 2
      ;;
    --only)
      ONLY="$2"
      shift 2
      ;;
    *)
      echo "Unknown arg: $1"
      exit 2
      ;;
  esac
done

# ---------------------------------------------------------------------------
# Test table: "app|expected_uart_string|timeout_s"
# ---------------------------------------------------------------------------
TESTS=(
  "uart_hello|hello, ra8d2!|10"
  "crc_demo|crc: selftest PASS|10"
  "dma_memcopy_demo|dma: copied 1024B match=Y|10"
  "adc_b_demo|adc: read PASS|10"
  "agt_periodic|agt: tick OK|10"
  "elc_event_demo|elc: en=1 trig=|10"
  "rng_demo|trng: entropy OK|10"
  "iwdt_demo|iwdt: poll counter|15"
  "ulpt_demo|ulpt: wake ok|15"
  "timer_capture_demo|gpt: period=|15"
  "threadx_ipc_demo|[ipc_demo] <- pong|15"
  "power_profiler|pp: profile OK|15"
  "lpm_idle_demo|lpm: wake_count=|15"
  "crypto_aes_demo|aes: round-trip OK|15"
  "watchdog_demo|wdt: boot reason=|15"
  "eth_loopback|etha: loopback ok|20"
  "sdram_benchmark|sdram: bench OK|20"
  "rtc_alarm|rtc: alarm fired|30"
)

UART_DIR="examples/ek_ra8d2/hw_validated/hil"

RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

pass=0
fail=0
declare -a failures=()

for entry in "${TESTS[@]}"; do
  IFS='|' read -r app expect timeout_s <<<"$entry"
  if [[ -n "$ONLY" && ",${ONLY}," != *",${app},"* ]]; then
    continue
  fi
  hex="${UART_DIR}/${app}/build/${app}.hex"

  printf "${YELLOW}[SUITE]${NC} %-35s expect='%s'\n" "${app}" "${expect}"

  if [[ ! -f "$hex" ]]; then
    printf "${RED}[SKIP]${NC}  %s -- hex not found: %s\n" "${app}" "${hex}"
    failures+=("${app} (no hex)")
    ((fail++)) || true
    continue
  fi

  # --uart is forwarded only when the caller named one; otherwise run_direct.sh
  # resolves the console by device identity, which is the right answer more
  # often than any ttyACM number is.
  uart_arg=()
  [[ -n "${UART}" ]] && uart_arg=(--uart "${UART}")

  if bash scripts/hil/run_direct.sh \
    --hex "${hex}" \
    --expect "${expect}" \
    --baud 115200 \
    --timeout "${timeout_s}" \
    "${uart_arg[@]}"; then
    ((pass++)) || true
  else
    failures+=("${app}")
    ((fail++)) || true
  fi
done

echo ""
echo "========================================"
printf "  HIL suite: %d passed, %d failed\n" "${pass}" "${fail}"
echo "========================================"
if [[ ${#failures[@]} -gt 0 ]]; then
  echo "  FAILED apps:"
  for f in "${failures[@]}"; do
    printf "    - %s\n" "${f}"
  done
  echo ""
  exit 1
fi
echo ""
exit 0
