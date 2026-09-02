#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# Record independently captured camera frames and encode them as an MP4.

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
  # shellcheck source=scripts/hil/lib/bench_lock.sh
  source "$ROOT/scripts/hil/lib/bench_lock.sh"
  FRAMES=3
  FPS=1
  OUTPUT="$PWD/camera_capture.mp4"

  usage() {
    echo "Usage: $0 [--frames N] [--fps N] [--output FILE]"
    exit 2
  }

  while [[ $# -gt 0 ]]; do
    case "$1" in
      --frames)
        [[ $# -ge 2 ]] || usage
        FRAMES="$2"
        shift 2
        ;;
      --fps)
        [[ $# -ge 2 ]] || usage
        FPS="$2"
        shift 2
        ;;
      --output)
        [[ $# -ge 2 ]] || usage
        OUTPUT="$2"
        shift 2
        ;;
      *) usage ;;
    esac
  done

  [[ "$FRAMES" =~ ^[1-9][0-9]*$ ]] || usage
  [[ "$FPS" =~ ^[1-9][0-9]*$ ]] || usage
  command -v ffmpeg >/dev/null || {
    echo "ffmpeg is required" >&2
    exit 2
  }

  FRAME_DIR="$(mktemp -d "${TMPDIR:-/tmp}/ra8-camera-video.XXXXXX")"
  trap 'rm -rf "$FRAME_DIR"' EXIT
  ra8_bench_require "record camera video" || exit $?

  for ((index = 1; index <= FRAMES; index += 1)); do
    printf 'capturing frame %u/%u\n' "$index" "$FRAMES"
    "$ROOT/scripts/hil/camera_picture.sh" --single \
      "$FRAME_DIR/frame_$(printf '%03u' "$index").ppm"
  done

  ffmpeg -hide_banner -loglevel error -y -framerate "$FPS" \
    -i "$FRAME_DIR/frame_%03d.ppm" -c:v libx264 -pix_fmt yuv420p "$OUTPUT"
  printf 'camera video saved: %s (%u frames at %u fps)\n' "$OUTPUT" "$FRAMES" "$FPS"
else
  [[ "$-" == *p* ]]
fi
