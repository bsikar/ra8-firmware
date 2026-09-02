#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# Capture one camera frame and dump four firmware-rotated RGB888 PPM files.

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

  ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
  # shellcheck source=scripts/hil/lib/rig_env.sh
  source "$ROOT/scripts/hil/lib/rig_env.sh"
  rig_require PI_HOST JLINK_SN JLINK_DEVICE PI_REPO
  : "${PI_HOST:?}"
  : "${JLINK_SN:?}"
  : "${JLINK_DEVICE:?}"
  : "${PI_REPO:?}"
  # shellcheck source=scripts/hil/lib/bench_lock.sh
  source "$ROOT/scripts/hil/lib/bench_lock.sh"
  APP="$ROOT/examples/ek_ra8d2/hw_validated/hil/camera_capture"
  BUILD_DIR="${CAMERA_BUILD_DIR:-$APP/build}"
  ELF="$BUILD_DIR/camera_capture.elf"
  HEX="$BUILD_DIR/camera_capture.hex"
  ALL_ROTATIONS=1
  SKIP_BUILD=0
  while [[ "${1:-}" == --* ]]; do
    case "$1" in
      --single)
        ALL_ROTATIONS=0
        ;;
      --skip-build)
        SKIP_BUILD=1
        ;;
      *)
        echo "usage: $0 [--single] [--skip-build] [output.ppm]" >&2
        exit 2
        ;;
    esac
    shift
  done
  OUTPUT="${1:-$PWD/camera_capture.ppm}"
  OUTPUT_STEM="${OUTPUT%.ppm}"
  RAW="${OUTPUT_STEM}.uyvy"
  LOCAL_RGB_DIR="$(mktemp -d "${TMPDIR:-/tmp}/ra8-camera-rgb.XXXXXX")"
  REMOTE_RAW="/tmp/camera_capture_${$}.uyvy"
  REMOTE_RGB_PREFIX="/tmp/camera_capture_${$}"
  REMOTE_SCRIPT="/tmp/camera_capture_${$}.jlink"
  REMOTE_LOG="/tmp/camera_capture_${$}.jlink.log"
  REMOTE_FILES_CREATED=0
  cleanup_local() {
    rm -rf "$LOCAL_RGB_DIR"
    return 0
  }
  cleanup_remote() {
    if [[ "$REMOTE_FILES_CREATED" != 1 ]]; then
      return 0
    fi
    # The process-specific remote paths are intentionally expanded by this client.
    if ssh -o BatchMode=yes -o ConnectTimeout=5 "$PI_HOST" "rm -f '${REMOTE_RAW}' \
    '${REMOTE_RGB_PREFIX}_0.rgb' '${REMOTE_RGB_PREFIX}_90.rgb' \
    '${REMOTE_RGB_PREFIX}_180.rgb' '${REMOTE_RGB_PREFIX}_270.rgb' \
    '${REMOTE_SCRIPT}' '${REMOTE_LOG}'"; then
      REMOTE_FILES_CREATED=0
    fi
    return 0
  }
  trap cleanup_local EXIT

  if [[ "$SKIP_BUILD" == 0 ]]; then
    cmake -S "$APP" -B "$BUILD_DIR" -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchain-ra8d2.cmake" \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build "$BUILD_DIR" --parallel 4
  else
    [[ -f "$ELF" && -f "$HEX" ]] || {
      echo "camera artifacts not found in $BUILD_DIR" >&2
      exit 2
    }
  fi
  ra8_bench_require "capture and dump camera frame" || exit $?
  # Run remote cleanup before the guard's EXIT handler releases the bench.
  _ra8_bench_add_exit_trap cleanup_remote

  symbol_addr() {
    arm-none-eabi-nm -n "$ELF" | awk -v symbol="$1" '$3 == symbol { print "0x" $1; exit }'
  }

  FRAME_ADDR="$(symbol_addr s_camera_capture)"
  RGB_0_ADDR="$(symbol_addr g_cam_rgb_0)"
  RGB_90_ADDR="$(symbol_addr g_cam_rgb_90)"
  RGB_180_ADDR="$(symbol_addr g_cam_rgb_180)"
  RGB_270_ADDR="$(symbol_addr g_cam_rgb_270)"
  for ADDRESS in "$FRAME_ADDR" "$RGB_0_ADDR" "$RGB_90_ADDR" "$RGB_180_ADDR" "$RGB_270_ADDR"; do
    [[ -n "$ADDRESS" ]] || {
      echo "camera image symbol not found in $ELF" >&2
      exit 2
    }
  done

  /bin/bash -p "$ROOT/scripts/hil/run_direct.sh" --hex "$HEX" --expect 'verdict=PASS' \
    --expect-negative 'verdict=FAIL' --timeout 30

  REMOTE_FILES_CREATED=1
  # The repository and process-specific output paths are client values.
  printf -v PI_REPO_Q '%q' "$PI_REPO"
  # shellcheck disable=SC2029  # client-selected repo/output paths expand before ssh.
  ssh "$PI_HOST" "cd ${PI_REPO_Q} && \
	cat > '${REMOTE_SCRIPT}' <<'JLINK'
device ${JLINK_DEVICE}
si SWD
speed 1000
connect
halt
savebin ${REMOTE_RAW} ${FRAME_ADDR} 0x96000
savebin ${REMOTE_RGB_PREFIX}_0.rgb ${RGB_0_ADDR} 0xE1000
savebin ${REMOTE_RGB_PREFIX}_90.rgb ${RGB_90_ADDR} 0xE1000
savebin ${REMOTE_RGB_PREFIX}_180.rgb ${RGB_180_ADDR} 0xE1000
savebin ${REMOTE_RGB_PREFIX}_270.rgb ${RGB_270_ADDR} 0xE1000
g
q
JLINK
JLinkExe -nogui 1 -SelectEmuBySN \"${JLINK_SN}\" -commanderscript '${REMOTE_SCRIPT}' > '${REMOTE_LOG}' 2>&1 && \
grep -q 'O.K.' '${REMOTE_LOG}' && \
test \"\$(stat -c %s '${REMOTE_RAW}')\" = 614400 && \
test \"\$(stat -c %s '${REMOTE_RGB_PREFIX}_0.rgb')\" = 921600 && \
test \"\$(stat -c %s '${REMOTE_RGB_PREFIX}_90.rgb')\" = 921600 && \
test \"\$(stat -c %s '${REMOTE_RGB_PREFIX}_180.rgb')\" = 921600 && \
test \"\$(stat -c %s '${REMOTE_RGB_PREFIX}_270.rgb')\" = 921600"

  scp -q "${PI_HOST}:${REMOTE_RAW}" "$RAW"
  scp -q "${PI_HOST}:${REMOTE_RGB_PREFIX}_0.rgb" "$LOCAL_RGB_DIR/0.rgb"
  if [[ "$ALL_ROTATIONS" == 1 ]]; then
    for ROTATION in 90 180 270; do
      scp -q "${PI_HOST}:${REMOTE_RGB_PREFIX}_${ROTATION}.rgb" "$LOCAL_RGB_DIR/${ROTATION}.rgb"
    done
  fi
  cleanup_remote

  write_ppm() {
    local source="$1"
    local destination="$2"
    local width="$3"
    local height="$4"
    {
      printf 'P6\n%s %s\n255\n' "$width" "$height"
      cat "$source"
    } >"$destination"
  }

  write_ppm "$LOCAL_RGB_DIR/0.rgb" "$OUTPUT" 640 480
  if [[ "$ALL_ROTATIONS" == 1 ]]; then
    write_ppm "$LOCAL_RGB_DIR/90.rgb" "${OUTPUT_STEM}_90.ppm" 480 640
    write_ppm "$LOCAL_RGB_DIR/180.rgb" "${OUTPUT_STEM}_180.ppm" 640 480
    write_ppm "$LOCAL_RGB_DIR/270.rgb" "${OUTPUT_STEM}_270.ppm" 480 640
  fi

  if [[ "$ALL_ROTATIONS" == 1 ]]; then
    printf 'firmware-rotated camera pictures saved:\n  %s (0/360 degrees)\n  %s_90.ppm\n  %s_180.ppm\n  %s_270.ppm\n' \
      "$OUTPUT" "$OUTPUT_STEM" "$OUTPUT_STEM" "$OUTPUT_STEM"
  else
    printf 'firmware camera picture saved: %s\n' "$OUTPUT"
  fi
  printf 'raw packed YCbCr 4:2:2 saved: %s\n' "$RAW"
else
  [[ "$-" == *p* ]]
fi
