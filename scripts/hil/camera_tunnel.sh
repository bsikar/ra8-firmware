#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# Expose the rig-only C6 camera HTTP server on this workstation.

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
  rig_require PI_HOST

  ARTIFACT_DIR="${C6_CAMERA_ARTIFACT_DIR:-/tmp/ra8-camera-livestream}"
  CAMERA_IP="${1:-${C6_CAMERA_IP:-}}"
  LOCAL_PORT="${2:-${C6_CAMERA_LOCAL_PORT:-8080}}"

  if [[ -z "$CAMERA_IP" && -f "$ARTIFACT_DIR/board-ip" ]]; then
    CAMERA_IP="$(tr -d '[:space:]' <"$ARTIFACT_DIR/board-ip")"
  fi
  if [[ ! "$CAMERA_IP" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "usage: just hil::camera_tunnel <camera-ip> [local-port]" >&2
    echo "       run 'just hil::c6 c6_camera_livestream' first to remember its DHCP address" >&2
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
else
  [[ "$-" == *p* ]]
fi
