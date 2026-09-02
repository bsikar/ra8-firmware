#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# rig_env.sh -- load protected HIL rig configuration without committing a
# maintainer-specific host or probe serial to the tree.
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

# Load the explicit config, checkout-local .env, or Ansible-provisioned user
# config (in that order). The selected file is data, never Bash code: the
# adjacent parser opens it without following the final symlink, authenticates
# current ownership and mode 0600 on that descriptor, and emits only the four
# declared rig values plus documented non-secret workstation controls through
# protected NUL-delimited pairs. The fixed case statement below, rather than
# `source` or `eval`, imports those values without losing embedded newlines.
_rig_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
_rig_lib_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Establish the value/default authority before any selected input is parsed.
# shellcheck source=scripts/hil/lib/rig_contract.sh
if ! source "$_rig_lib_dir/rig_contract.sh"; then
  printf 'error: rig contract requires a privileged Bash caller\n' >&2
  return 2
fi

_rig_env_file="${RA8_RIG_ENV:-}"
if [[ -n "${_rig_env_file}" && ! -e "${_rig_env_file}" && ! -L "${_rig_env_file}" ]]; then
  printf 'error: RA8_RIG_ENV does not exist: %s\n' "${_rig_env_file}" >&2
  return 2
fi
if [[ -z "${_rig_env_file}" &&
  (-e "${_rig_root}/.env" || -L "${_rig_root}/.env") ]]; then
  _rig_env_file="${_rig_root}/.env"
fi
_rig_config_home="${XDG_CONFIG_HOME:-}"
if [[ -z "${_rig_config_home}" && -n "${HOME:-}" ]]; then
  _rig_config_home="${HOME}/.config"
fi
if [[ -z "${_rig_env_file}" && -n "${_rig_config_home}" &&
  (-e "${_rig_config_home}/ra8/hil.env" || -L "${_rig_config_home}/ra8/hil.env") ]]; then
  _rig_env_file="${_rig_config_home}/ra8/hil.env"
fi

PI_HOST="${PI_HOST:-}"
JLINK_SN="${JLINK_SN:-}"
JLINK_DEVICE="${JLINK_DEVICE:-$(ra8_rig_contract_default JLINK_DEVICE)}"
PI_REPO="${PI_REPO:-$(ra8_rig_contract_default PI_REPO)}"
RA8_BENCH_WAIT="${RA8_BENCH_WAIT:-}"
RA8_BENCH_WAIT_S="${RA8_BENCH_WAIT_S:-}"
RA8_BENCH_ACTORS="${RA8_BENCH_ACTORS:-}"
RA8_CONSOLE_TTY="${RA8_CONSOLE_TTY:-}"
C6_CONSOLE_TTY="${C6_CONSOLE_TTY:-}"

if [[ -n "${_rig_env_file}" ]]; then
  _rig_parser="$_rig_root/scripts/hil/rig_env_parse.py"
  _rig_python="$(type -P python3 || true)"
  [[ -n "$_rig_python" && -x "$_rig_python" && -f "$_rig_parser" ]] || {
    printf 'error: protected rig environment parser is unavailable\n' >&2
    return 2
  }
  _rig_result_dir="$(mktemp -d "${TMPDIR:-/tmp}/ra8-rig-env.XXXXXX")" || return 2
  chmod 0700 "$_rig_result_dir" || {
    rmdir "$_rig_result_dir" 2>/dev/null || true
    return 2
  }
  _rig_result="$_rig_result_dir/values.tsv"
  (umask 077 && : >"$_rig_result") || {
    rmdir "$_rig_result_dir" 2>/dev/null || true
    return 2
  }
  if ! "$_rig_python" -I "$_rig_parser" --input "$_rig_env_file" \
    --output "$_rig_result" --format nul --include-interactive >/dev/null; then
    rm -f "$_rig_result"
    rmdir "$_rig_result_dir" 2>/dev/null || true
    return 2
  fi
  _rig_import_ok=true
  while IFS= read -r -d '' _rig_name && IFS= read -r -d '' _rig_value; do
    case "$_rig_name" in
      C6_CONSOLE_TTY) C6_CONSOLE_TTY="$_rig_value" ;;
      PI_HOST) PI_HOST="$_rig_value" ;;
      JLINK_SN) JLINK_SN="$_rig_value" ;;
      JLINK_DEVICE) JLINK_DEVICE="$_rig_value" ;;
      PI_REPO) PI_REPO="$_rig_value" ;;
      RA8_BENCH_ACTORS) RA8_BENCH_ACTORS="$_rig_value" ;;
      RA8_BENCH_WAIT) RA8_BENCH_WAIT="$_rig_value" ;;
      RA8_BENCH_WAIT_S) RA8_BENCH_WAIT_S="$_rig_value" ;;
      RA8_CONSOLE_TTY) RA8_CONSOLE_TTY="$_rig_value" ;;
      *)
        printf 'error: protected rig parser returned an unknown field\n' >&2
        _rig_import_ok=false
        break
        ;;
    esac
  done <"$_rig_result"
  rm -f "$_rig_result"
  rmdir "$_rig_result_dir" || return 2
  [[ "$_rig_import_ok" == true ]] || return 2
fi
export C6_CONSOLE_TTY PI_HOST JLINK_SN JLINK_DEVICE PI_REPO
export RA8_BENCH_ACTORS RA8_BENCH_WAIT RA8_BENCH_WAIT_S RA8_CONSOLE_TTY
unset _rig_config_home _rig_env_file _rig_import_ok _rig_name _rig_parser _rig_python
unset _rig_result _rig_result_dir _rig_root _rig_value

# Serial-console resolution by device identity, never by ttyACM number.
# shellcheck source=scripts/hil/lib/tty_resolve.sh
. "$_rig_lib_dir/tty_resolve.sh"

# RA8_TTY_RESOLVER_SRC -- the resolver's own text, for the scripts whose UART
# work happens on the far side of an ssh. They paste this into the remote
# heredoc so the Pi resolves the console with the identical function this
# machine would use, without needing a checkout there.
# shellcheck disable=SC2034  # consumed by the scripts that source this file (run.sh, check_alive.sh, recover.sh), never here.
RA8_TTY_RESOLVER_SRC="$(cat "$_rig_lib_dir/tty_resolve.sh")"

# Validate every non-empty/defaulted value immediately, including PI_HOST in
# scripts where it is optional and therefore never reaches rig_require. Empty
# required coordinates remain allowed until a consumer explicitly requires
# them, preserving local-only find/probe workflows.
if ! ra8_rig_validate_loaded true; then
  return 2
fi

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

# rig_require VAR... -- validate and require declared typed coordinates.
rig_require() {
  ra8_rig_require "$@" || exit 2
}

unset _rig_lib_dir
