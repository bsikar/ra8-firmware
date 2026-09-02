#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# hil_dev.sh -- Full HIL suite runner for the dev machine.
#
# Builds every discovered HIL app locally (this machine has the cross
# toolchain; the bench host does not), ships the hexes and the scripts/hil tree
# to the bench, and runs scripts/hil/all.sh there. Mirrors what CI does on the
# self-hosted runner.
#
# Usage (run from repo root on dev):
#   /bin/bash -p scripts/hil/dev.sh [--only app1,app2]

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

  ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
  # Resolve GCC and binutils through the same 13.3 pin as CI and CMake. A stale
  # unversioned ~/opt/arm-gnu-toolchain path must never select HIL codegen.
  # shellcheck source=scripts/ci/lib/arm_toolchain.sh
  source "$ROOT/scripts/ci/lib/arm_toolchain.sh"
  use_pinned_arm_toolchain
  require_pinned_arm_toolchain
  # Rig config (PI_HOST) comes from the gitignored .env, not the tree.
  _hil_dir="$(dirname "${BASH_SOURCE[0]}")"
  _hil_dir="$(cd "$_hil_dir" && pwd)"
  # shellcheck source=scripts/hil/lib/rig_env.sh
  source "$_hil_dir/lib/rig_env.sh"
  rig_require PI_HOST JLINK_SN

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

  UART_DIR="examples/ek_ra8d2/hw_validated/hil"

  # The app list comes from the filesystem, via the SAME discovery the suite and
  # the EIL runner use. It used to be a hand-written array of 21 names here and a
  # second hand-written table of 18 in a now-deleted runner, against a much larger
  # discovered set -- so `just hil` tested only a fraction of the tree and every
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
  declare -a HIL_BUILD_TARGETS=()
  for app in "${HIL_APPS[@]}"; do
    HIL_BUILD_TARGETS+=("${UART_DIR#examples/}/$app")
  done
  printf '%s\0' "${HIL_BUILD_TARGETS[@]}" |
    RA8_SELECTED_APP_FLOOR="${#HIL_BUILD_TARGETS[@]}" BUILD_TYPE=RelWithDebInfo \
      /bin/bash -p "$ROOT/scripts/builders/all_examples.sh" --selected0

  # ---- bench mutual exclusion --------------------------------------------------
  # Local compilation does not touch the rig, so finish it before occupying the
  # physical bench. One live hold then spans staging and the complete remote run.
  # shellcheck source=scripts/hil/lib/bench_lock.sh
  source "$_hil_dir/lib/bench_lock.sh"
  ra8_bench_require "HIL suite from this workstation${ONLY:+ (only $ONLY)}" 2h || exit $?

  echo "[hil-dev] Creating workspace on bench host: $REMOTE_DIR"
  # shellcheck disable=SC2029  # $REMOTE_DIR is chosen locally; the Pi cannot know it.
  ssh "$PI" "mkdir -p $REMOTE_DIR/scripts"

  # shellcheck disable=SC2329  # invoked by the EXIT trap installed below
  cleanup_remote() {
    echo "[hil-dev] Cleaning up remote workspace..."
    # shellcheck disable=SC2029  # $REMOTE_DIR is chosen locally; the Pi cannot know it.
    ssh "$PI" "rm -rf $REMOTE_DIR" >/dev/null 2>&1 || true
  }
  trap cleanup_remote EXIT

  echo "[hil-dev] Uploading scripts..."
  # The whole scripts/hil tree, at its real path: all.sh invokes
  # `scripts/hil/run_direct.sh` by that path, and run_direct.sh sources
  # `lib/rig_env.sh` (and through it lib/tty_resolve.sh) from beside itself.
  # Uploading the two files flat left both of those dangling.
  scp -q -r "$ROOT/scripts/hil" "$PI:$REMOTE_DIR/scripts/"

  echo "[hil-dev] Uploading manifests and firmware artifacts..."
  declare -a STAGE_FILES=(
    scripts/dev/ra8_apps.py
    scripts/checks/check_no_antirecovery.py
    scripts/checks/check_image_no_antirecovery.py
    scripts/ci/lib/arm_toolchain.sh
  )
  for app in "${HIL_APPS[@]}"; do
    cmake_file="$UART_DIR/$app/CMakeLists.txt"
    main_file="$UART_DIR/$app/src/main.c"
    conf="$UART_DIR/$app/hil.conf"
    hex="$UART_DIR/$app/build/$app.hex"
    elf="$UART_DIR/$app/build/$app.elf"
    STAGE_FILES+=("$cmake_file" "$main_file" "$conf")
    if [[ -f "$ROOT/$hex" ]]; then
      STAGE_FILES+=("$hex")
    else
      echo "[hil-dev] WARNING: hex not found for $app, skipping"
    fi
    if [[ -f "$ROOT/$elf" ]]; then
      STAGE_FILES+=("$elf")
    fi
  done
  # shellcheck disable=SC2029  # REMOTE_DIR is a locally generated /tmp path with a numeric suffix
  COPYFILE_DISABLE=1 tar --no-xattrs -C "$ROOT" -cf - "${STAGE_FILES[@]}" |
    ssh "$PI" "tar -xf - -C $REMOTE_DIR"

  echo "[hil-dev] Running the suite on the bench host..."
  # No --uart: the Pi resolves the board console by device identity
  # (scripts/hil/lib/tty_resolve.sh). Naming a ttyACM number from here was how a
  # renumbered bench got pointed at the wrong device.
  #
  # --skip-build because the hexes were just built HERE and shipped above. The
  # bench host still resolves its provisioned 13.3 toolchain for the binutils
  # used by image validation and per-mode verifiers; it does not rebuild them.
  # RA8_BENCH_LOCK_ID carries this script's hold across the ssh, so the suite
  # runs inside it instead of trying to take a second one.
  printf -v _lock_q '%q' "${RA8_BENCH_LOCK_ID:-}"
  printf -v _jlink_sn_q '%q' "$JLINK_SN"
  printf -v _pi_host_q '%q' "$PI_HOST"
  SUITE_CMD="cd $REMOTE_DIR && RA8_BENCH_LOCK_ID=${_lock_q} JLINK_SN=${_jlink_sn_q} PI_HOST=${_pi_host_q} /bin/bash -p scripts/hil/all.sh --skip-build"
  if [[ -n "$ONLY" ]]; then
    printf -v _only_q '%q' "$ONLY"
    SUITE_CMD+=" --only ${_only_q}"
  fi

  # shellcheck disable=SC2029  # $SUITE_CMD is the command this script composed locally to run there.
  if ssh "$PI" "$SUITE_CMD"; then
    rc=0
  else
    rc=$?
  fi

  exit "$rc"
else
  [[ "$-" == *p* ]]
fi
