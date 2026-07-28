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
#   PI_REPO       path to the checkout ON the bench Pi, relative to the ssh
#                 login directory or absolute (default ra8-firmware). Only
#                 run_direct.sh needs it: it re-invokes itself there by piping
#                 itself into `bash -s`, and the piped copy resolves lib/ from
#                 the working directory. Everything else ships what it needs
#                 inline (see RA8_TTY_RESOLVER_SRC below).
#
# Sourcing this also brings in `ra8_tty_resolve` (lib/tty_resolve.sh), because
# every consumer of the rig needs to name a console and none of them may name
# it by ttyACM number.
#
# Portability: pure bash 3.2 (the macOS system bash) -- no name-refs, no mapfile.

# Load .env if present, exported so child processes (JLinkExe, rfp-cli, ssh
# remotes) inherit the values.
_rig_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
if [ -f "$_rig_root/.env" ]; then
  set -a
  # shellcheck disable=SC1091  # path is the repo-root .env, resolved above.
  . "$_rig_root/.env"
  set +a
fi
unset _rig_root

# Serial-console resolution by device identity, never by ttyACM number.
# shellcheck source=scripts/hil/lib/tty_resolve.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/tty_resolve.sh"

# RA8_TTY_RESOLVER_SRC -- the resolver's own text, for the scripts whose UART
# work happens on the far side of an ssh. They paste this into the remote
# heredoc so the Pi resolves the console with the identical function this
# machine would use, without needing a checkout there.
# shellcheck disable=SC2034  # consumed by the scripts that source this file (run.sh, check_alive.sh, recover.sh), never here.
RA8_TTY_RESOLVER_SRC="$(cat "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/tty_resolve.sh")"

# Declare the .env-provided contract explicitly. These are set by the `. .env`
# above when it exists; defaulting them to empty here keeps every consumer safe
# under `set -u` (an absent .env must reach rig_require's helpful message, not
# an unbound-variable abort) and states the interface in one place.
PI_HOST="${PI_HOST:-}"
JLINK_SN="${JLINK_SN:-}"

# The device name is not maintainer-specific; default it here, allow override.
JLINK_DEVICE="${JLINK_DEVICE:-R7KA8D2KF_CPU0}"

# Where the bench Pi keeps its own checkout. Not maintainer-specific enough to
# demand a .env entry, but overridable there when it is not the default.
PI_REPO="${PI_REPO:-ra8-firmware}"

# Bare Pi hostname (strip "user@" and ".local") for the run-on-the-Pi check.
PI_BAREHOST="${PI_HOST##*@}"
PI_BAREHOST="${PI_BAREHOST%%.*}"

# rig_is_local_pi -- succeed (return 0) when this script is running ON the bench
# Pi itself, so callers run JLinkExe/rfp-cli locally instead of over ssh. True
# when the hostname matches PI_HOST's bare host, or on any aarch64 box with a
# CDC device attached. The second test used to require /dev/ttyACM0 by name,
# which made "am I on the bench" depend on USB enumeration order: after a power
# cycle that started numbering at ttyACM1, an aarch64 bench stopped recognising
# itself.
rig_is_local_pi() {
  local _h _d
  _h="$(hostname 2>/dev/null || true)"
  if [ -n "$PI_BAREHOST" ] &&
    { [ "$_h" = "$PI_BAREHOST" ] || [ "$_h" = "${PI_BAREHOST}-desktop" ]; }; then
    return 0
  fi
  [ "$(uname -m)" = "aarch64" ] || return 1
  for _d in /dev/ttyACM*; do
    [ -e "$_d" ] && return 0
  done
  return 1
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
