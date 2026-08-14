#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Expose the rig-only C6 camera HTTP server on this workstation.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=scripts/hil/lib/rig_env.sh
# shellcheck disable=SC1091
source "$ROOT/scripts/hil/lib/rig_env.sh"
rig_require PI_HOST

ARTIFACT_DIR="${C6_CAMERA_ARTIFACT_DIR:-/tmp/ra8-camera-livestream}"
CAMERA_IP="${1:-${C6_CAMERA_IP:-}}"
LOCAL_PORT="${2:-${C6_CAMERA_LOCAL_PORT:-8080}}"

if [[ -z "$CAMERA_IP" && -f "$ARTIFACT_DIR/board-ip" ]]; then
  CAMERA_IP="$(tr -d '[:space:]' <"$ARTIFACT_DIR/board-ip")"
fi
if [[ ! "$CAMERA_IP" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "usage: make hil-camera-tunnel IP=<camera-ip> [PORT=8080]" >&2
  echo "       run 'make hil-c6 APP=c6_camera_livestream' first to remember its DHCP address" >&2
  exit 2
fi
if [[ ! "$LOCAL_PORT" =~ ^[0-9]+$ ]] || ((LOCAL_PORT < 1 || LOCAL_PORT > 65535)); then
  echo "camera_tunnel: PORT must be between 1 and 65535" >&2
  exit 2
fi

echo "camera_tunnel: forwarding http://127.0.0.1:${LOCAL_PORT}/ -> http://${CAMERA_IP}/ via ${PI_HOST}" >&2
echo "camera_tunnel: press Ctrl-C to stop" >&2
exec ssh -N -o ExitOnForwardFailure=yes \
  -L "127.0.0.1:${LOCAL_PORT}:${CAMERA_IP}:80" "$PI_HOST"
