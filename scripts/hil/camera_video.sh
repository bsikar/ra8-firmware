#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Record independently captured camera frames and encode them as an MP4.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=scripts/hil/lib/bench_lock.sh
# shellcheck disable=SC1091
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
