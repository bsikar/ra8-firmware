#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# hil_dev.sh -- Full HIL suite runner for the dev machine.
#
# Builds every discovered HIL app locally (this machine has the cross
# toolchain; the bench host does not), ships the hexes and the scripts/hil tree
# to the bench, and runs scripts/hil/all.sh there. Mirrors what CI does on the
# self-hosted runner.
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

# ---- bench mutual exclusion --------------------------------------------------
# ONE hold across build + upload + run, taken here and carried over the ssh, so
# the whole suite is a single occupancy of the bench. After argument parsing, so
# a typo does not take the bench on its way to a usage error.
# shellcheck source=scripts/hil/lib/bench_lock.sh
source "$_hil_dir/lib/bench_lock.sh"
ra8_bench_require "HIL suite from this workstation${ONLY:+ (only $ONLY)}" 2h || exit $?

UART_DIR="examples/ek_ra8d2/hw_validated/hil"

# The app list comes from the filesystem, via the SAME discovery the suite and
# the SIL runner use. It used to be a hand-written array of 21 names here and a
# second hand-written table of 18 in a now-deleted runner, against 151 apps
# that actually exist -- so `make hil` tested an eighth of the tree and every
# app added since had to be remembered into two lists. One source of truth:
# hil_discover_apps().
# shellcheck source=scripts/hil/lib/hil_conf.sh
source "$_hil_dir/lib/hil_conf.sh"
declare -a HIL_APPS=()
while IFS= read -r _app; do
  [[ -n "$ONLY" && ",${ONLY}," != *",${_app},"* ]] && continue
  HIL_APPS+=("$_app")
done < <(hil_discover_apps "$ROOT/$UART_DIR")

((${#HIL_APPS[@]} > 0)) || {
  echo "[hil-dev] no apps discovered under $UART_DIR"
  exit 1
}

echo "[hil-dev] Building ${#HIL_APPS[@]} discovered HIL apps..."
make -j"$(nproc)" -C "$ROOT" "${HIL_APPS[@]}"

echo "[hil-dev] Creating workspace on star: $REMOTE_DIR"
# shellcheck disable=SC2029  # $REMOTE_DIR is chosen locally; the Pi cannot know it.
ssh "$PI" "mkdir -p $REMOTE_DIR/scripts"

echo "[hil-dev] Uploading scripts..."
# The whole scripts/hil tree, at its real path: all.sh invokes
# `scripts/hil/run_direct.sh` by that path, and run_direct.sh sources
# `lib/rig_env.sh` (and through it lib/tty_resolve.sh) from beside itself.
# Uploading the two files flat left both of those dangling.
scp -q -r "$ROOT/scripts/hil" "$PI:$REMOTE_DIR/scripts/"

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

echo "[hil-dev] Running the suite on the bench host..."
# No --uart: the Pi resolves the board console by device identity
# (scripts/hil/lib/tty_resolve.sh). Naming a ttyACM number from here was how a
# renumbered bench got pointed at the wrong device.
#
# --skip-build because the hexes were just built HERE, with this machine's
# toolchain, and shipped above; the bench host has no cross-compiler.
# RA8_BENCH_LOCK_ID carries this script's hold across the ssh, so the suite
# runs inside it instead of trying to take a second one.
SUITE_CMD="cd $REMOTE_DIR && RA8_BENCH_LOCK_ID=${RA8_BENCH_LOCK_ID:-} bash scripts/hil/all.sh --skip-build"
[[ -n "$ONLY" ]] && SUITE_CMD+=" --only $ONLY"

# shellcheck disable=SC2029  # $SUITE_CMD is the command this script composed locally to run there.
ssh "$PI" "$SUITE_CMD"
rc=$?

echo "[hil-dev] Cleaning up remote workspace..."
# shellcheck disable=SC2029  # $REMOTE_DIR is chosen locally; the Pi cannot know it.
ssh "$PI" "rm -rf $REMOTE_DIR"

exit "$rc"
