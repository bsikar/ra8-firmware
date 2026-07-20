#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# rig_env.sh -- load HIL rig configuration from the gitignored repo-root .env
# so no maintainer-specific host or probe serial is committed to the tree.
# `source` this (do not execute it) near the top of a HIL script, then call
# `rig_require` for the variables that script actually uses.
#
# Variables (set them in .env; copy .env.example to start):
#   PI_HOST       ssh target for the bench Pi, e.g. user@host.local
#   JLINK_SN      on-board J-Link OB serial (see .env.example for how to find it)
#   JLINK_DEVICE  Renesas device name (default R7KA8D2KF_CPU0; rarely changed)
#
# Portability: pure bash 3.2 (the macOS system bash) -- no name-refs, no mapfile.

# Load .env if present, exported so child processes (JLinkExe, rfp-cli, ssh
# remotes) inherit the values.
_rig_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
if [ -f "$_rig_root/.env" ]; then
  set -a
  # shellcheck disable=SC1091  # path is the repo-root .env, resolved above.
  . "$_rig_root/.env"
  set +a
fi
unset _rig_root

# Declare the .env-provided contract explicitly. These are set by the `. .env`
# above when it exists; defaulting them to empty here keeps every consumer safe
# under `set -u` (an absent .env must reach rig_require's helpful message, not
# an unbound-variable abort) and states the interface in one place.
PI_HOST="${PI_HOST:-}"
JLINK_SN="${JLINK_SN:-}"

# The device name is not maintainer-specific; default it here, allow override.
JLINK_DEVICE="${JLINK_DEVICE:-R7KA8D2KF_CPU0}"

# Bare Pi hostname (strip "user@" and ".local") for the run-on-the-Pi check.
PI_BAREHOST="${PI_HOST##*@}"
PI_BAREHOST="${PI_BAREHOST%%.*}"

# rig_is_local_pi -- succeed (return 0) when this script is running ON the bench
# Pi itself, so callers run JLinkExe/rfp-cli locally instead of over ssh. True
# when the hostname matches PI_HOST's bare host, or on any aarch64 box with the
# probe enumerated at /dev/ttyACM0.
rig_is_local_pi() {
  local _h
  _h="$(hostname 2>/dev/null || true)"
  if [ -n "$PI_BAREHOST" ] &&
    { [ "$_h" = "$PI_BAREHOST" ] || [ "$_h" = "${PI_BAREHOST}-desktop" ]; }; then
    return 0
  fi
  [ -e /dev/ttyACM0 ] && [ "$(uname -m)" = "aarch64" ]
}

# rig_require VAR...  -- exit 2 with a helpful message if any named var is empty.
rig_require() {
  local _n _missing=0
  for _n in "$@"; do
    if [ -z "${!_n:-}" ]; then
      printf 'error: %s is not set. Copy .env.example to .env and fill it in.\n' "$_n" >&2
      _missing=1
    fi
  done
  [ "$_missing" -eq 0 ] || exit 2
}
