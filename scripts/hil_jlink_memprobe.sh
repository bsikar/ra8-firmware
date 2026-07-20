#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# hil_jlink_memprobe.sh -- flash a firmware app, halt twice via J-Link
# with a sample window between halts, and assert that a named symbol
# (typically a `volatile uint32_t` counter incremented in the app's
# main loop / ISR) has advanced by at least N between the two halts.
#
# Used for apps that have no UART (or shouldn't, because adding UART
# would balloon the firmware footprint -- blink, blink_hal,
# threadx_blink). The pure HIL_MODE=alive check only proves CycleCnt
# advances and PC is not in a fault spinner; it does NOT prove that
# THIS app's specific main loop iterated. A counter symbol bridges
# the gap: if the counter advances by >=N over the sample window, the
# app's expected work happened.
#
# Usage:
#   bash scripts/hil_jlink_memprobe.sh --hex <file.hex> \
#                                      --symbol <name>     \
#                                      [--min-advance N]   \
#                                      [--seconds N]       \
#                                      [--app-name <name>]
#
# Per-app hil.conf:
#   HIL_MODE=jlink_memprobe
#   HIL_PROBE_SYMBOL="g_blink_tick"      # name of `volatile uint32_t`
#   HIL_PROBE_MIN_ADVANCE=4              # minimum delta over the window
#   HIL_PROBE_SECONDS=3                  # sample window length
#
# Exit:
#   0 = symbol advanced by >= MIN_ADVANCE within SECONDS
#   1 = symbol did NOT advance enough, or J-Link error
#   2 = usage error / Pi unreachable

set -euo pipefail

# Rig config (PI_HOST, JLINK_SN) comes from the gitignored .env, not the tree.
_hil_dir="$(dirname "${BASH_SOURCE[0]}")"
_hil_dir="$(cd "$_hil_dir" && pwd)"
# shellcheck source=scripts/lib/rig_env.sh
source "$_hil_dir/lib/rig_env.sh"
rig_require PI_HOST JLINK_SN

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

HEX=""
SYMBOL=""
MIN_ADVANCE=4
SECONDS_S=3
APP_NAME=""
FAILURE_SYMBOL=""
MAX_FAILURE_ADVANCE=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --hex)
      HEX="$2"
      shift 2
      ;;
    --symbol)
      SYMBOL="$2"
      shift 2
      ;;
    --min-advance)
      MIN_ADVANCE="$2"
      shift 2
      ;;
    --seconds)
      SECONDS_S="$2"
      shift 2
      ;;
    --app-name)
      APP_NAME="$2"
      shift 2
      ;;
    --failure-symbol)
      FAILURE_SYMBOL="$2"
      shift 2
      ;;
    --max-failure-advance)
      MAX_FAILURE_ADVANCE="$2"
      shift 2
      ;;
    -h | --help)
      sed -n '5,30p' "$0"
      exit 0
      ;;
    *)
      echo "Unknown arg: $1"
      exit 2
      ;;
  esac
done

[[ -n "$HEX" ]] || {
  echo "Usage: $0 --hex <file> --symbol <name> [--min-advance N] [--seconds N]"
  exit 2
}
[[ -n "$SYMBOL" ]] || {
  echo "Usage: $0 --hex <file> --symbol <name>"
  exit 2
}
[[ -f "$HEX" ]] || {
  echo -e "${RED}[ERROR]${NC} hex not found: $HEX"
  exit 1
}
[[ -z "$APP_NAME" ]] && APP_NAME="$(basename "${HEX%.hex}")"

ELF="${HEX%.hex}.elf"
[[ -f "$ELF" ]] || {
  echo -e "${RED}[ERROR]${NC} .elf needed alongside .hex to resolve symbol address"
  exit 1
}
command -v arm-none-eabi-nm >/dev/null 2>&1 ||
  {
    echo -e "${RED}[ERROR]${NC} arm-none-eabi-nm not in PATH"
    exit 1
  }

# Resolve the symbol address from the .elf. Reject if the symbol is
# undefined, weak, or in a discarded section -- those would silently
# read 0xFFFFFFFF or constant zero.
#
# awk must NOT 'exit' on the first match: nm keeps writing, so an early
# exit closes the pipe and nm dies with SIGPIPE -- which, under
# 'set -o pipefail', fails the whole script. Match-and-latch instead so
# awk drains all of nm's output.
SYM_LINE="$(arm-none-eabi-nm --print-size "$ELF" | awk -v s="$SYMBOL" '$NF==s && !f {print; f=1}')"
if [[ -z "$SYM_LINE" ]]; then
  echo -e "${RED}[memprobe]${NC} symbol '${SYMBOL}' not found in $(basename "$ELF")"
  exit 1
fi
SYM_ADDR="$(echo "$SYM_LINE" | awk '{print $1}')"
SYM_TYPE="$(echo "$SYM_LINE" | awk '{print $(NF-1)}')"
# Type 'd' / 'D' = data, 'b' / 'B' = bss. Both fine. Reject 't'/'T' (code).
if [[ ! "$SYM_TYPE" =~ ^[bBdD]$ ]]; then
  echo -e "${RED}[memprobe]${NC} symbol '${SYMBOL}' type '${SYM_TYPE}' is not data (need bss/data, not text)"
  exit 1
fi

# Same lookup for the optional failure-counter symbol.
FAIL_ADDR=""
if [[ -n "$FAILURE_SYMBOL" ]]; then
  FAIL_LINE="$(arm-none-eabi-nm --print-size "$ELF" | awk -v s="$FAILURE_SYMBOL" '$NF==s && !f {print; f=1}')"
  if [[ -z "$FAIL_LINE" ]]; then
    echo -e "${RED}[memprobe]${NC} failure-symbol '${FAILURE_SYMBOL}' not found in $(basename "$ELF")"
    exit 1
  fi
  FAIL_ADDR="$(echo "$FAIL_LINE" | awk '{print $1}')"
  FAIL_TYPE="$(echo "$FAIL_LINE" | awk '{print $(NF-1)}')"
  if [[ ! "$FAIL_TYPE" =~ ^[bBdD]$ ]]; then
    echo -e "${RED}[memprobe]${NC} failure-symbol '${FAILURE_SYMBOL}' type '${FAIL_TYPE}' is not data"
    exit 1
  fi
fi

echo -e "${YELLOW}[memprobe]${NC} app=${APP_NAME} symbol=${SYMBOL} addr=0x${SYM_ADDR} min_advance=${MIN_ADVANCE} window=${SECONDS_S}s"
if [[ -n "$FAIL_ADDR" ]]; then
  echo -e "${YELLOW}[memprobe]${NC} failure_symbol=${FAILURE_SYMBOL} addr=0x${FAIL_ADDR} max_failure_advance=${MAX_FAILURE_ADVANCE}"
fi

# 1. Flash.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"${SCRIPT_DIR}/hil_flash.sh" "$APP_NAME" >/dev/null || {
  echo -e "${RED}[memprobe]${NC} flash failed for ${APP_NAME}"
  exit 1
}

# 2. Run on the Pi (or locally if already on it): halt, read symbol,
# go, sleep, halt, read symbol again. Sleep is the SECONDS_S sample
# window during which the chip runs freely and the counter ticks.
RUN_LOCAL=0
if rig_is_local_pi; then
  RUN_LOCAL=1
fi

PI_TMP="/tmp/hil_memprobe_${APP_NAME}.jlink"
PI_LOG="/tmp/hil_memprobe_${APP_NAME}.log"
JLINK_SCRIPT_PRE=$(
  cat <<EOF
device ${JLINK_DEVICE}
si SWD
speed 1000
connect
halt
mem32 0x${SYM_ADDR} 1
EOF
)
if [[ -n "$FAIL_ADDR" ]]; then
  JLINK_SCRIPT_PRE="$(printf '%s\nmem32 0x%s 1' "$JLINK_SCRIPT_PRE" "$FAIL_ADDR")"
fi
JLINK_SCRIPT_POST=$(
  cat <<EOF
go
sleep $((SECONDS_S * 1000))
halt
mem32 0x${SYM_ADDR} 1
EOF
)
if [[ -n "$FAIL_ADDR" ]]; then
  JLINK_SCRIPT_POST="$(printf '%s\nmem32 0x%s 1' "$JLINK_SCRIPT_POST" "$FAIL_ADDR")"
fi
JLINK_SCRIPT="$(printf '%s\n%s\nq\n' "$JLINK_SCRIPT_PRE" "$JLINK_SCRIPT_POST")"

if ((RUN_LOCAL)); then
  echo "$JLINK_SCRIPT" >"$PI_TMP"
  JLinkExe -nogui 1 -SelectEmuBySN "${JLINK_SN}" -commanderscript "$PI_TMP" >"$PI_LOG" 2>&1 || true
else
  # shellcheck disable=SC2029  # ${PI_TMP} is the locally-chosen remote path for the J-Link script.
  ssh "$PI_HOST" "cat > ${PI_TMP}" <<<"$JLINK_SCRIPT"
  # shellcheck disable=SC2029  # ${JLINK_SN}/${PI_TMP}/${PI_LOG} are local rig config and local path choices.
  ssh "$PI_HOST" "JLinkExe -nogui 1 -SelectEmuBySN ${JLINK_SN} -commanderscript ${PI_TMP} > ${PI_LOG} 2>&1 || true"
  scp -q "${PI_HOST}:${PI_LOG}" "${PI_LOG}" 2>/dev/null || true
fi

# 3. Parse mem32 outputs. J-Link prints each as "ADDR = VALUE " on its
# own line. Pull values for the primary symbol; if a failure symbol
# is set, pull those too.
mapfile -t MEM_HITS < <(grep -iE "^${SYM_ADDR}" "$PI_LOG" | awk '{print $3}' | tr 'a-f' 'A-F')
if [[ ${#MEM_HITS[@]} -lt 2 ]]; then
  echo -e "${RED}[memprobe]${NC} failed to read symbol twice from J-Link log:"
  tail -20 "$PI_LOG" >&2
  exit 1
fi
PRE="${MEM_HITS[0]}"
POST="${MEM_HITS[1]}"
pre_dec=$((16#$PRE))
post_dec=$((16#$POST))
delta=$((post_dec - pre_dec))
if ((delta < 0)); then
  delta=$((((1 << 32) - pre_dec) + post_dec))
fi

# Failure-symbol parse: should advance no more than MAX_FAILURE_ADVANCE.
fail_delta=0
FAIL_PRE=""
FAIL_POST=""
if [[ -n "$FAIL_ADDR" ]]; then
  mapfile -t FAIL_HITS < <(grep -iE "^${FAIL_ADDR}" "$PI_LOG" | awk '{print $3}' | tr 'a-f' 'A-F')
  if [[ ${#FAIL_HITS[@]} -lt 2 ]]; then
    echo -e "${RED}[memprobe]${NC} failed to read failure-symbol twice from J-Link log"
    tail -20 "$PI_LOG" >&2
    exit 1
  fi
  FAIL_PRE="${FAIL_HITS[0]}"
  FAIL_POST="${FAIL_HITS[1]}"
  fail_pre_dec=$((16#$FAIL_PRE))
  fail_post_dec=$((16#$FAIL_POST))
  fail_delta=$((fail_post_dec - fail_pre_dec))
  if ((fail_delta < 0)); then
    fail_delta=$((((1 << 32) - fail_pre_dec) + fail_post_dec))
  fi
fi

ok=1
if ((delta < MIN_ADVANCE)); then
  ok=0
fi
if [[ -n "$FAIL_ADDR" ]] && ((fail_delta > MAX_FAILURE_ADVANCE)); then
  ok=0
fi

PRIMARY_MSG="${SYMBOL}: 0x${PRE} -> 0x${POST}, delta=${delta} (need >= ${MIN_ADVANCE})"
FAIL_MSG=""
if [[ -n "$FAIL_ADDR" ]]; then
  FAIL_MSG="; ${FAILURE_SYMBOL}: 0x${FAIL_PRE} -> 0x${FAIL_POST}, delta=${fail_delta} (max ${MAX_FAILURE_ADVANCE})"
fi

if ((ok == 1)); then
  echo -e "${GREEN}[memprobe]${NC} ${APP_NAME} PASS (${PRIMARY_MSG}${FAIL_MSG})"
  exit 0
else
  echo -e "${RED}[memprobe]${NC} ${APP_NAME} FAIL (${PRIMARY_MSG}${FAIL_MSG})"
  echo "    (J-Link log: ${PI_LOG})"
  exit 1
fi
