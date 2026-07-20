#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/lib/hil_conf.sh -- shared discovery + hil.conf sourcing for the HIL
# (hardware-in-the-loop) and SIL (simulator-in-the-loop) suites.
#
# Both scripts/hil_all.sh (flash a real board, scrape its UART) and
# scripts/sil_all.sh (boot the same .elf in tools/board_sim, scrape the
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
# the single source of truth so hil_all.sh and sil_all.sh reset and export the
# exact same set (a leaked value from a previous app's manifest is a silent
# cross-contamination bug). Grep-derived from
# examples/ek_ra8d2/hw_validated/hil/*/hil.conf; add new knobs here.
#
# Two SIL-only knobs, ignored by hil_all.sh and applied only by sil_all.sh:
#   HIL_SIM_ARGS        extra board_sim flags an app needs under emulation
#                       (e.g. `--device ra8p1` for an RA8P1/NPU app).
#   HIL_SIM_MAX_CHUNKS  per-app instruction-chunk cap override -- raise it for a
#                       compute-heavy app whose banner lands past the default cap
#                       (e.g. a software-crypto KAT) so the global cap can stay
#                       low and a genuinely-stuck app fails fast.
HIL_CONF_VARS="
  HIL_MODE
  HIL_EXPECT HIL_EXPECT_NEGATIVE HIL_EXPECT_SHORT_OK HIL_EXPECT_OVERLAP_OK
  HIL_TIMEOUT_S HIL_SIM_ARGS HIL_SIM_MAX_CHUNKS
  HIL_VIDPID HIL_HUB_PORT HIL_PPPS_MODE HIL_MPS_CHUNK HIL_STREAM_BYTES HIL_STREAM_FLOOR_KBS
  HIL_BOOT_S HIL_FAULT_EXPECTED
  HIL_PROBE_SYMBOL HIL_PROBE_MIN_ADVANCE HIL_PROBE_SECONDS HIL_PROBE_BOOT_S
  HIL_PROBE_FAILURE_SYMBOL HIL_PROBE_MAX_FAILURE
  HIL_BOARD_IP HIL_PORT HIL_PROTO HIL_PAYLOAD_BYTES HIL_BOOT_TIMEOUT_S HIL_PROBE_TIMEOUT_S
  HIL_RTT_BUF_SYMBOL HIL_RTT_BUF_BYTES
  HIL_POST_INITIALIZE
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
HIL_TIMEOUT_S="${HIL_TIMEOUT_S:-}"
HIL_SIM_ARGS="${HIL_SIM_ARGS:-}"
HIL_SIM_MAX_CHUNKS="${HIL_SIM_MAX_CHUNKS:-}"
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
HIL_POST_INITIALIZE="${HIL_POST_INITIALIZE:-}"

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
# the whole set so child processes (per-mode runners, board_sim workers) inherit
# them. Must be called in the shell that will read the variables -- NOT in a
# `$(...)` subshell.
hil_conf_load() {
  local conf="$1" v
  # `$HIL_CONF_VARS` is deliberately word-split into individual names.
  for v in $HIL_CONF_VARS; do
    unset "$v"
  done
  # allexport: every variable the manifest assigns is auto-exported, so child
  # processes (per-mode runners, board_sim workers) inherit exactly the knobs
  # this app declared -- no need to enumerate the export list a second time.
  set -a
  # shellcheck disable=SC1090  # path is a runtime argument.
  . "$conf"
  set +a
}
