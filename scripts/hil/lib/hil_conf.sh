#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# scripts/hil/lib/hil_conf.sh -- shared discovery + hil.conf sourcing for the HIL
# (hardware-in-the-loop) and EIL (emulator-in-the-loop) suites.
#
# Both scripts/hil/all.sh (flash a real board, scrape its UART) and
# scripts/emu/eil_all.sh (boot the same .elf in tools/ra8_emulator, scrape the
# emulated UART) discover the SAME apps under
# examples/ek_ra8d2/hw_validated/hil/ and read the SAME per-app hil.conf
# manifests. Factoring the two shared steps here keeps the two suites in
# lockstep: an app added to hil/, or a new HIL_* knob added to a hil.conf,
# is picked up by both without editing each script.
#
# This file is meant to be `source`d, not executed. It defines:
#   HIL_CONF_VARS       -- the authoritative list of hil.conf variable names.
#   hil_discover_apps   -- print the app names under a hil/ directory.
#   hil_conf_load       -- reset + source + export one app's hil.conf.
#
# Portability: pure bash 3.2 (the macOS system bash) -- no name-refs, no
# mapfile. Lists are returned on stdout, one item per line.

# Authoritative list of every variable a hil.conf may declare. Kept here as
# the single source of truth so hil_all.sh and eil_all.sh reset and export the
# exact same set (a leaked value from a previous app's manifest is a silent
# cross-contamination bug). Grep-derived from
# examples/ek_ra8d2/hw_validated/hil/*/hil.conf; add new knobs here.
#
# Two EIL-only knobs, ignored by hil_all.sh and applied only by eil_all.sh:
#   HIL_EMU_ARGS        extra ra8_emulator flags an app needs under emulation
#                       (e.g. `--device ra8p1` for an RA8P1/NPU app).
#   HIL_EMU_MAX_CHUNKS  per-app instruction-chunk cap override -- raise it for a
#                       compute-heavy app whose banner lands past the default cap
#                       (e.g. a software-crypto KAT) so the global cap can stay
#                       low and a genuinely-stuck app fails fast.
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
    if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then
      printf 'error: sourced privileged entry refuses inherited Bash functions\n' >&2
      unset -v ra8_startup_env_done_count
      unset -v ra8_startup_env_name ra8_startup_env_row ra8_startup_env_unset
      unset -v RA8_STARTUP_ENV_DONE RA8_STARTUP_ENV_SCRUBBED
      unset -f _ra8_startup_refuse
      return 2
    fi
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

  HIL_CONF_VARS="
  HIL_MODE
  HIL_EXPECT HIL_EXPECT_NEGATIVE HIL_EXPECT_SHORT_OK HIL_EXPECT_OVERLAP_OK
  HIL_PROVISION_WIFI
  HIL_TIMEOUT_S HIL_EMU_ARGS HIL_EMU_MAX_CHUNKS
  HIL_VIDPID HIL_HUB_PORT HIL_PPPS_MODE HIL_MPS_CHUNK HIL_STREAM_BYTES HIL_STREAM_FLOOR_KBS
  HIL_BOOT_S HIL_FAULT_EXPECTED
  HIL_PROBE_SYMBOL HIL_PROBE_MIN_ADVANCE HIL_PROBE_SECONDS HIL_PROBE_BOOT_S
  HIL_PROBE_FAILURE_SYMBOL HIL_PROBE_MAX_FAILURE
  HIL_BOARD_IP HIL_PORT HIL_PROTO HIL_PAYLOAD_BYTES HIL_BOOT_TIMEOUT_S HIL_PROBE_TIMEOUT_S
  HIL_RTT_BUF_SYMBOL HIL_RTT_BUF_BYTES
  HIL_SELF_BUILD HIL_FRAME_WIDTH HIL_FRAME_HEIGHT
  HIL_POST_INITIALIZE HIL_POST_POWER_CYCLE_HALT
"

  # Declare every knob above with an empty default. hil_conf_load resets and
  # re-sources these per app, but declaring them here states the contract in one
  # place and keeps every consumer safe under `set -u` when an app's hil.conf
  # omits a knob. Keep this list in step with HIL_CONF_VARS above.
  HIL_MODE="${HIL_MODE:-}"
  HIL_EXPECT="${HIL_EXPECT:-}"
  HIL_EXPECT_NEGATIVE="${HIL_EXPECT_NEGATIVE:-}"
  HIL_EXPECT_SHORT_OK="${HIL_EXPECT_SHORT_OK:-}"
  HIL_EXPECT_OVERLAP_OK="${HIL_EXPECT_OVERLAP_OK:-}"
  HIL_PROVISION_WIFI="${HIL_PROVISION_WIFI:-}"
  HIL_TIMEOUT_S="${HIL_TIMEOUT_S:-}"
  HIL_EMU_ARGS="${HIL_EMU_ARGS:-}"
  HIL_EMU_MAX_CHUNKS="${HIL_EMU_MAX_CHUNKS:-}"
  HIL_VIDPID="${HIL_VIDPID:-}"
  HIL_HUB_PORT="${HIL_HUB_PORT:-}"
  HIL_PPPS_MODE="${HIL_PPPS_MODE:-}"
  HIL_MPS_CHUNK="${HIL_MPS_CHUNK:-}"
  HIL_STREAM_BYTES="${HIL_STREAM_BYTES:-}"
  HIL_STREAM_FLOOR_KBS="${HIL_STREAM_FLOOR_KBS:-}"
  HIL_BOOT_S="${HIL_BOOT_S:-}"
  HIL_FAULT_EXPECTED="${HIL_FAULT_EXPECTED:-}"
  HIL_PROBE_SYMBOL="${HIL_PROBE_SYMBOL:-}"
  HIL_PROBE_MIN_ADVANCE="${HIL_PROBE_MIN_ADVANCE:-}"
  HIL_PROBE_SECONDS="${HIL_PROBE_SECONDS:-}"
  HIL_PROBE_BOOT_S="${HIL_PROBE_BOOT_S:-}"
  HIL_PROBE_FAILURE_SYMBOL="${HIL_PROBE_FAILURE_SYMBOL:-}"
  HIL_PROBE_MAX_FAILURE="${HIL_PROBE_MAX_FAILURE:-}"
  HIL_BOARD_IP="${HIL_BOARD_IP:-}"
  HIL_PORT="${HIL_PORT:-}"
  HIL_PROTO="${HIL_PROTO:-}"
  HIL_PAYLOAD_BYTES="${HIL_PAYLOAD_BYTES:-}"
  HIL_BOOT_TIMEOUT_S="${HIL_BOOT_TIMEOUT_S:-}"
  HIL_PROBE_TIMEOUT_S="${HIL_PROBE_TIMEOUT_S:-}"
  HIL_RTT_BUF_SYMBOL="${HIL_RTT_BUF_SYMBOL:-}"
  HIL_RTT_BUF_BYTES="${HIL_RTT_BUF_BYTES:-}"
  HIL_SELF_BUILD="${HIL_SELF_BUILD:-}"
  HIL_FRAME_WIDTH="${HIL_FRAME_WIDTH:-}"
  HIL_FRAME_HEIGHT="${HIL_FRAME_HEIGHT:-}"
  HIL_POST_INITIALIZE="${HIL_POST_INITIALIZE:-}"
  HIL_POST_POWER_CYCLE_HALT="${HIL_POST_POWER_CYCLE_HALT:-}"

  # hil_discover_apps <hil_dir>
  #
  # Print, one per line and sorted, the name of every app directory directly
  # under <hil_dir>. A directory is an app iff it is an immediate child dir; the
  # README.md file is skipped. Apps WITHOUT a hil.conf are still printed -- the
  # caller decides how to treat a missing manifest (hil_all.sh fails loud).
  hil_discover_apps() {
    local hil_dir="$1" d name
    [ -d "$hil_dir" ] || return 0
    while IFS= read -r d; do
      name="$(basename "$d")"
      [ "$name" = "README.md" ] && continue
      printf '%s\n' "$name"
    done < <(find "$hil_dir" -mindepth 1 -maxdepth 1 -type d | sort)
  }

  # hil_conf_load <conf_path>
  #
  # Reset every known HIL_* variable (so a value from a previously-loaded
  # manifest cannot leak), source <conf_path> into the current shell, then export
  # the whole set so child processes (per-mode runners, ra8_emulator workers) inherit
  # them. Must be called in the shell that will read the variables -- NOT in a
  # `$(...)` subshell.
  hil_conf_load() {
    local conf="$1" v allexport_was_set=0
    # `$HIL_CONF_VARS` is deliberately word-split into individual names.
    for v in $HIL_CONF_VARS; do
      unset "$v"
    done
    # A caller may itself be using allexport. Disable it while reading the
    # manifest so unknown or misspelled assignments do not leak to subprocesses,
    # then restore the caller's shell state exactly.
    case $- in
      *a*)
        allexport_was_set=1
        set +a
        ;;
    esac
    # shellcheck disable=SC1090  # path is a runtime argument.
    . "$conf"
    for v in $HIL_CONF_VARS; do
      export "${v?}"
    done
    if ((allexport_was_set)); then
      set -a
    fi
  }

  hil_conf_selftest() (
    set -euo pipefail
    local tmp conf
    tmp="$(mktemp -d "${TMPDIR:-/tmp}/ra8-hil-conf.XXXXXX")"
    trap 'rm -rf "$tmp"' EXIT
    conf="$tmp/hil.conf"
    printf '%s\n' 'HIL_MODE=uart_scrape' 'RA8_HIL_UNKNOWN=must_not_export' >"$conf"

    set +a
    hil_conf_load "$conf"
    [[ "$HIL_MODE" == uart_scrape ]]
    env | grep -q '^HIL_MODE=uart_scrape$'
    if env | grep -q '^RA8_HIL_UNKNOWN='; then
      return 1
    fi
    case $- in
      *a*) return 1 ;;
    esac

    unset RA8_HIL_UNKNOWN
    set -a
    hil_conf_load "$conf"
    case $- in
      *a*) ;;
      *) return 1 ;;
    esac
    if env | grep -q '^RA8_HIL_UNKNOWN='; then
      return 1
    fi
    printf '%s\n' 'hil_conf.sh --selftest: PASS (known-only export and allexport preservation)'
  )

  if [[ "${BASH_SOURCE[0]:-}" == "$0" ]]; then
    [[ "${1:-}" == "--selftest" && $# -eq 1 ]] || {
      echo "usage: $0 --selftest" >&2
      exit 2
    }
    hil_conf_selftest
  fi
else
  [[ "$-" == *p* ]]
fi
