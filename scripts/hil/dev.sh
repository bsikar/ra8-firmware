#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# hil_dev.sh -- Full HIL suite runner for the dev machine.
#
# Builds all 20 uart apps locally, ships hexes + suite scripts to star,
# and runs hil_suite.sh there. Mirrors what CI does on the self-hosted runner.
#
# Usage (run from repo root on dev):
#   bash scripts/hil/dev.sh [--only app1,app2]

set -euo pipefail

export PATH="$HOME/opt/arm-gnu-toolchain/bin:$PATH"

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
# Rig config (PI_HOST) comes from the gitignored .env, not the tree.
_hil_dir="$(dirname "${BASH_SOURCE[0]}")"
_hil_dir="$(cd "$_hil_dir" && pwd)"
# shellcheck source=scripts/hil/lib/rig_env.sh
source "$_hil_dir/lib/rig_env.sh"
rig_require PI_HOST
PI="$PI_HOST"
REMOTE_DIR="/tmp/hil-dev-$$"
ONLY=""

while [[ $# -gt 0 ]]; do
  case "$1" in
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

HIL_APPS=(
  uart_hello crc_demo dma_memcopy_demo
  adc_b_demo agt_periodic elc_event_demo rng_demo
  iwdt_demo ulpt_demo timer_capture_demo threadx_ipc_demo
  power_profiler lpm_idle_demo crypto_aes_demo
  watchdog_demo eth_loopback sdram_benchmark rtc_alarm
  canfd_loopback can_classic_loopback spi_loopback
)

UART_DIR="examples/ek_ra8d2/hw_validated/hil"

echo "[hil-dev] Building ${#HIL_APPS[@]} uart apps..."
make -j"$(nproc)" -C "$ROOT" "${HIL_APPS[@]}"

echo "[hil-dev] Creating workspace on star: $REMOTE_DIR"
# shellcheck disable=SC2029  # $REMOTE_DIR is chosen locally; the Pi cannot know it.
ssh "$PI" "mkdir -p $REMOTE_DIR/scripts"

echo "[hil-dev] Uploading scripts..."
scp "$ROOT/scripts/hil/suite.sh" "$ROOT/scripts/hil/run_direct.sh" \
  "$PI:$REMOTE_DIR/scripts/"

echo "[hil-dev] Uploading hex files..."
for app in "${HIL_APPS[@]}"; do
  hex="$ROOT/$UART_DIR/$app/build/$app.hex"
  if [[ -f "$hex" ]]; then
    # shellcheck disable=SC2029  # $REMOTE_DIR/$UART_DIR/$app are local values naming the remote staging dir.
    ssh "$PI" "mkdir -p $REMOTE_DIR/$UART_DIR/$app/build"
    scp "$hex" "$PI:$REMOTE_DIR/$UART_DIR/$app/build/$app.hex"
  else
    echo "[hil-dev] WARNING: hex not found for $app, skipping"
  fi
done

echo "[hil-dev] Running suite on star..."
SUITE_CMD="cd $REMOTE_DIR && bash scripts/hil/suite.sh --uart /dev/ttyACM0"
[[ -n "$ONLY" ]] && SUITE_CMD+=" --only $ONLY"

# shellcheck disable=SC2029  # $SUITE_CMD is the command this script composed locally to run there.
ssh "$PI" "$SUITE_CMD"
rc=$?

echo "[hil-dev] Cleaning up remote workspace..."
# shellcheck disable=SC2029  # $REMOTE_DIR is chosen locally; the Pi cannot know it.
ssh "$PI" "rm -rf $REMOTE_DIR"

exit "$rc"
