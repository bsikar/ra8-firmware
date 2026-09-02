#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# hil_dlm_reset_local.sh -- Mac-direct (no-SSH) sibling of
# hil_dlm_reset.sh. Recovers an EK-RA8D2 wired straight into THIS host's
# USB (J-Link OB enumerated locally) back to OEM_PL2 by invoking the
# boot-firmware Initialize command via `rfp-cli -erase-chip`, and also
# clears any TrustZone Boundary (IDAU partition) back to the all-Secure
# default.
#
# Why a separate script: hil_dlm_reset.sh shells out to the bench Pi
# over `ssh $PI_HOST`. When the board is cabled directly to a
# laptop (no Pi in the loop) that path cannot reach the probe. This
# script runs rfp-cli locally instead, and resolves the rfp-cli binary
# from a few well-known install locations because Renesas Flash
# Programmer does not put itself on PATH.
#
# Prereqs (same hardware contract as hil_dlm_reset.sh):
#   1. The chip's `ce` Security Flag ("Disable Initialize Command")
#      must NOT have been set. EK-RA8D2 ships with `Security Flags:
#      none`, so Initialize is available unless YOU set `ce`.
#   2. The SWD-DP must still respond. If the chip is in LCK_BOOT
#      (SWD-DP fully unresponsive) nothing here -- or anywhere -- can
#      recover it.
#
# Modes:
#   check    -- READ-ONLY. Run `-rfo` and print DLM State / Security
#               Flags / Boundary. Touches nothing. Use this to prove the
#               recovery path can reach the board BEFORE a risky option-
#               byte program.
#   recover  -- DESTRUCTIVE on user flash. Run `-erase-chip` (boot-
#               firmware Initialize): erases all blocks, clears flash
#               options (so Boundary returns to the all-Secure default),
#               and regresses DLM OEM_PL0/PL1 -> OEM_PL2. Then re-reads
#               and verifies DLM == OEM_PL2.
#
# Usage:
#   /bin/bash -p scripts/hil/dlm_reset_local.sh check
#   /bin/bash -p scripts/hil/dlm_reset_local.sh recover
#
# Exit codes:
#   0  -- check succeeded, or recover landed at OEM_PL2 / Flags none
#   1  -- rfp-cli error, or recover did not reach OEM_PL2
#   2  -- usage error / rfp-cli binary not found

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

  set -uo pipefail

  # Rig config (JLINK_SN) comes from the gitignored .env, not the tree.
  _hil_dir="$(dirname "${BASH_SOURCE[0]}")"
  _hil_dir="$(cd "$_hil_dir" && pwd)"
  # shellcheck source=scripts/hil/lib/rig_env.sh
  source "$_hil_dir/lib/rig_env.sh"
  rig_require JLINK_SN
  DEVICE="${RFP_DEVICE:-ra}"
  SPEED="${RFP_SPEED:-1000000}"

  GREEN='\033[0;32m'
  RED='\033[0;31m'
  YELLOW='\033[1;33m'
  CYAN='\033[0;36m'
  NC='\033[0m'

  tag() { printf "${CYAN}[dlm_local]${NC} %s\n" "$*"; }
  ok() { printf "${GREEN}[OK]${NC}  %s\n" "$*"; }
  err() { printf "${RED}[FAIL]${NC} %s\n" "$*" >&2; }
  warn() { printf "${YELLOW}[WARN]${NC} %s\n" "$*"; }

  # ---- Resolve rfp-cli (RFP does not add itself to PATH) ----------------------
  resolve_rfp() {
    if [[ -n "${RFP_CLI:-}" && -x "${RFP_CLI}" ]]; then
      echo "${RFP_CLI}"
      return 0
    fi
    if command -v rfp-cli >/dev/null 2>&1; then
      command -v rfp-cli
      return 0
    fi
    local cand
    for cand in \
      "$HOME/opt/rfp/rfp-cli" \
      "/opt/rfp/rfp-cli" \
      "/opt/rfp/linux-x64/rfp-cli" \
      "/Applications/Renesas Flash Programmer.app/Contents/MacOS/rfp-cli"; do
      [[ -x "$cand" ]] && {
        echo "$cand"
        return 0
      }
    done
    return 1
  }

  RFP="$(resolve_rfp)" || {
    err "rfp-cli not found. Set RFP_CLI=/path/to/rfp-cli or install Renesas"
    err "Flash Programmer. Looked in PATH, \$HOME/opt/rfp, /opt/rfp."
    exit 2
  }

  rfo() { # rfo <logfile> -- read flash options, capture to log
    "$RFP" -d "$DEVICE" -t "jlink:${JLINK_SN}" -if swd -s "$SPEED" -rfo \
      >"$1" 2>&1
  }

  print_state() { # print_state <logfile> <label>
    local f="$1" label="$2" st fl bd
    st="$(grep -E '^DLM State:' "$f" | head -1 | sed 's/^DLM State: //')"
    fl="$(grep -E '^Security Flags:' "$f" | head -1 | sed 's/^Security Flags: //')"
    bd="$(grep -E '^Boundary:' "$f" | head -1 | sed 's/^Boundary: //')"
    tag "${label}: DLM=${st:-<none>}  Flags=${fl:-<none>}  Boundary=${bd:-<none>}"
  }

  MODE="${1:-}"
  [[ "$MODE" == "check" || "$MODE" == "recover" ]] || {
    echo "Usage: $0 {check|recover}" >&2
    exit 2
  }

  tag "rfp-cli: ${RFP}"
  tag "J-Link SN: ${JLINK_SN}  device: ${DEVICE}  speed: ${SPEED} Hz"

  # ---- check: READ-ONLY -------------------------------------------------------
  if [[ "$MODE" == "check" ]]; then
    LOG="/tmp/hil_dlm_local_check.log"
    tag "Reading flash options (READ-ONLY)..."
    if ! rfo "$LOG"; then
      err "rfp-cli -rfo failed. Output:"
      tail -15 "$LOG" | sed 's/^/    /' >&2
      exit 1
    fi
    if ! grep -q "DLM State:" "$LOG"; then
      err "rfo did not return a DLM state. Output:"
      tail -15 "$LOG" | sed 's/^/    /' >&2
      exit 1
    fi
    print_state "$LOG" "STATE"
    ok "Recovery path reachable -- check log: ${LOG}"
    exit 0
  fi

  # ---- recover: DESTRUCTIVE ---------------------------------------------------
  BEFORE="/tmp/hil_dlm_local_before.log"
  INIT="/tmp/hil_dlm_local_init.log"
  AFTER="/tmp/hil_dlm_local_after.log"

  tag "Reading DLM state BEFORE Initialize..."
  rfo "$BEFORE" || true
  if grep -q "DLM State:" "$BEFORE"; then
    print_state "$BEFORE" "BEFORE"
  else
    warn "BEFORE: rfo did not return a readable DLM state (chip may be"
    warn "        debug-gated in PL0/PL1 -- expected before recovery)."
  fi

  tag "Invoking Initialize via rfp-cli -erase-chip..."
  tag "(Erases user flash AND clears flash options -> Boundary returns to"
  tag " the all-Secure default; regresses OEM_PL0/PL1 -> OEM_PL2.)"
  if ! "$RFP" -d "$DEVICE" -t "jlink:${JLINK_SN}" -if swd -s "$SPEED" \
    -erase-chip >"$INIT" 2>&1; then
    err "rfp-cli -erase-chip returned non-zero. Output:"
    tail -15 "$INIT" | sed 's/^/    /' >&2
    exit 1
  fi
  if ! grep -q "Operation successful" "$INIT"; then
    err "rfp-cli -erase-chip did NOT report Operation successful. Output:"
    tail -15 "$INIT" | sed 's/^/    /' >&2
    exit 1
  fi
  ok "Initialize command reported success."

  tag "Reading DLM state AFTER Initialize..."
  rfo "$AFTER" || true
  if ! grep -q "DLM State:" "$AFTER"; then
    err "AFTER: rfo did not return a DLM state. Output:"
    tail -15 "$AFTER" | sed 's/^/    /' >&2
    err "Recovery FAILED -- chip may be in LCK_BOOT or $(ce) may be set."
    exit 1
  fi
  print_state "$AFTER" "AFTER"

  AFTER_STATE="$(grep -E '^DLM State:' "$AFTER" | head -1 | sed 's/^DLM State: //')"
  if [[ "$AFTER_STATE" == "OEM_PL2" ]]; then
    ok "Chip recovered. DLM is at OEM_PL2; Boundary cleared to default."
    ok "Do NOT chain 'rfp-cli -dlm <state>' -- that would re-lock the chip."
    tag "Logs: BEFORE=${BEFORE}  INIT=${INIT}  AFTER=${AFTER}"
    tag "Next: re-flash a sanity app, e.g. scripts/hil/run_local.sh blink"
    exit 0
  fi

  err "Initialize ran but DLM state is now '${AFTER_STATE}' (expected OEM_PL2)."
  exit 1
else
  [[ "$-" == *p* ]]
fi
