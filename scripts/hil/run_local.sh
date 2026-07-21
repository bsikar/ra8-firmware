#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# hil_run_local.sh -- run a single app's HIL gate entirely on the developer's
# Mac, with the EK-RA8D2 plugged directly into this machine (no Pi).
#
# The bench Pi runners (hil_run_direct.sh / hil_jlink_memprobe.sh /
# hil_check_alive.sh) flash + verify on the bench Pi (`$PI_HOST`) and read
# /dev/ttyACM0 with Linux-only tools (udevadm, setsid, stdbuf, stty -F,
# timeout). This runner mirrors their exact J-Link command sequences and
# pass/fail logic, but drives the local J-Link OB over USB and reads the
# board's VCOM at /dev/cu.usbmodem* using only macOS-available tools
# (stty -f, a small unbuffered python3 reader). It reads each app's
# hil.conf and dispatches on HIL_MODE:
#
#   uart_scrape    -- flash, scrape the VCOM for HIL_EXPECT within
#                     HIL_TIMEOUT_S; fail if HIL_EXPECT_NEGATIVE matches.
#   jlink_memprobe -- flash, double-halt mem32 read of HIL_PROBE_SYMBOL
#                     (and optional HIL_PROBE_FAILURE_SYMBOL) across a
#                     HIL_PROBE_SECONDS window; assert the advance bounds.
#   alive          -- flash, dwell HIL_BOOT_S, then snapshot PC / CFSR /
#                     HFSR / CycleCnt via J-Link and reject faults or a
#                     PC parked in a known fault spinner.
#
# Usage:
#   scripts/hil/run_local.sh <app> [--uart /dev/cu.usbmodemXXXX]
#
# Exit codes:
#   0  PASS
#   1  FAIL (gate failed, or flash failed)
#   2  ERROR (usage / app not found / no probe / no VCOM)

set -euo pipefail

# Rig config (JLINK_SN) comes from the gitignored .env, not the tree.
_hil_dir="$(dirname "${BASH_SOURCE[0]}")"
_hil_dir="$(cd "$_hil_dir" && pwd)"
# shellcheck source=scripts/hil/lib/rig_env.sh
source "$_hil_dir/lib/rig_env.sh"
rig_require JLINK_SN
JLINK_DEVICE="R7KA8D2KF_CPU0"
JLINK_SPEED="1000"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

usage() {
  echo "Usage: $0 <app> [--uart /dev/cu.usbmodemXXXX]"
  exit 2
}

APP=""
UART=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --uart)
      UART="$2"
      shift 2
      ;;
    -h | --help)
      sed -n '5,30p' "$0"
      exit 0
      ;;
    -*)
      echo "Unknown arg: $1"
      usage
      ;;
    *)
      if [[ -n "$APP" ]]; then usage; fi
      APP="$1"
      shift
      ;;
  esac
done
[[ -n "$APP" ]] || usage

command -v JLinkExe >/dev/null 2>&1 || {
  echo -e "${RED}[local]${NC} JLinkExe not in PATH"
  exit 2
}
command -v arm-none-eabi-objcopy >/dev/null 2>&1 || {
  echo -e "${RED}[local]${NC} arm-none-eabi-objcopy not in PATH"
  exit 2
}
command -v python3 >/dev/null 2>&1 || {
  echo -e "${RED}[local]${NC} python3 not in PATH"
  exit 2
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# ---- Resolve the app dir / hex (build if missing) ---------------------------
# Process substitution + break (no `| head`): a `find | head` pipeline trips
# SIGPIPE on the upstream, and with `set -o pipefail` that fails the whole
# pipeline and `set -e` would exit the script before printing anything.
APP_DIR=""
while IFS= read -r d; do
  if [[ -f "$d/main.c" && "$(basename "$d")" == "$APP" ]]; then
    APP_DIR="$d"
    break
  fi
done < <(find "$ROOT/examples" -type d -name "$APP")
[[ -n "$APP_DIR" ]] || {
  echo -e "${RED}[local]${NC} app '${APP}' not found under examples/"
  exit 2
}

HEX="${APP_DIR}/build/${APP}.hex"
ELF="${APP_DIR}/build/${APP}.elf"
if [[ ! -f "$HEX" || ! -f "$ELF" ]]; then
  echo -e "${YELLOW}[local]${NC} building ${APP}..."
  make -C "$ROOT" "$APP"
fi
[[ -f "$HEX" && -f "$ELF" ]] || {
  echo -e "${RED}[local]${NC} build did not produce ${APP}.{hex,elf}"
  exit 1
}

# ---- Load the per-app hil.conf ----------------------------------------------
CONF="${APP_DIR}/hil.conf"
[[ -f "$CONF" ]] || {
  echo -e "${RED}[local]${NC} no hil.conf in ${APP_DIR}"
  exit 2
}
HIL_MODE="" HIL_EXPECT="" HIL_EXPECT_NEGATIVE="" HIL_TIMEOUT_S=""
HIL_BOOT_S="" HIL_PROBE_SYMBOL="" HIL_PROBE_MIN_ADVANCE="" HIL_PROBE_SECONDS=""
HIL_PROBE_FAILURE_SYMBOL="" HIL_PROBE_MAX_FAILURE="" HIL_PROBE_BOOT_S=""
# shellcheck disable=SC1090  # CONF path is computed at runtime; location is intentionally non-constant
source "$CONF"
MODE="${HIL_MODE:-}"
[[ -n "$MODE" ]] || {
  echo -e "${RED}[local]${NC} hil.conf has no HIL_MODE"
  exit 2
}

# ---- Detect the J-Link OB VCOM ----------------------------------------------
need_uart() {
  if [[ -z "$UART" ]]; then
    for d in /dev/cu.usbmodem*; do [[ -e "$d" ]] && {
      UART="$d"
      break
    }; done
  fi
  [[ -n "$UART" && -e "$UART" ]] || {
    echo -e "${RED}[local]${NC} no /dev/cu.usbmodem* VCOM found"
    exit 2
  }
}

# ---- Unbuffered serial reader (background) -----------------------------------
# macOS has no `stdbuf`/`setsid`; a small python3 reader writes every chunk
# to the log immediately so one-shot boot banners are never lost in a stdio
# buffer. /dev/cu.* is the call-out device -- open() does not block on DCD.
READER_PY="/tmp/hil_local_reader.py"
cat >"$READER_PY" <<'PY'
import os, sys, time, select, termios
port, dur, out = sys.argv[1], float(sys.argv[2]), sys.argv[3]
fd = os.open(port, os.O_RDONLY | os.O_NONBLOCK)
# Configure baud/raw on the fd we hold open. `stty -f` configures then
# closes the port; on macOS a subsequent open() resets the line to the
# default (9600), so the banner comes back as framing garbage. Setting
# termios here, on the live fd, makes 115200/8N1 raw actually stick.
a = termios.tcgetattr(fd)
a[0] = 0                                  # iflag
a[1] = 0                                  # oflag
a[3] = 0                                  # lflag (raw: no echo/canon/sig)
a[2] = (a[2] & ~termios.CSIZE) | termios.CS8
a[2] |= (termios.CLOCAL | termios.CREAD)
a[2] &= ~(termios.PARENB | termios.CSTOPB)
a[4] = termios.B115200                    # ispeed
a[5] = termios.B115200                    # ospeed
termios.tcsetattr(fd, termios.TCSANOW, a)
deadline = time.time() + dur
with open(out, "wb", buffering=0) as o:
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 0.2)
        if not r:
            continue
        try:
            data = os.read(fd, 4096)
        except (BlockingIOError, OSError):
            data = b""
        if data:
            o.write(data)
os.close(fd)
PY

READER_PID=""
start_reader() { # start_reader <log> <seconds>
  : >"$1"
  python3 "$READER_PY" "$UART" "$2" "$1" &
  READER_PID=$!
  sleep 0.3 # let it open the port before the board boots
}
stop_reader() {
  if [[ -n "$READER_PID" ]]; then
    kill "$READER_PID" 2>/dev/null || true # may have self-exited at its deadline
    wait "$READER_PID" 2>/dev/null || true
  fi
  READER_PID=""
}

# ---- OFS strip + flash via local JLinkExe -----------------------------------
# OFS sections at 0x0300A100+ trip the RAMCode Prepare() timeout; strip to
# the MRAM bank. PRCR-unlock + LPSCR clear (w2/w1/w2) defends against an app
# that parked the chip in software standby before WFI (harmless otherwise).
flash_local() {
  local stripped="/tmp/hil_local_${APP}_mram.hex"
  local log="/tmp/hil_local_${APP}_flash.log"
  # Two-project TrustZone apps (a sibling <app>_ns.elf exists) merge their
  # Secure + Non-Secure images into <app>.hex at build time; flash that
  # directly. Otherwise objcopy the single ELF, stripping option-setting
  # records the MRAM flasher cannot write.
  if [[ -f "${APP_DIR}/build/${APP}_ns.elf" ]]; then
    cp "$HEX" "$stripped"
  else
    arm-none-eabi-objcopy --remove-section='.option_setting*' -O ihex "$ELF" "$stripped" 2>/dev/null ||
      cp "$HEX" "$stripped"
  fi
  local jl="/tmp/hil_local_${APP}_flash.jlink"
  cat >"$jl" <<EOF
device ${JLINK_DEVICE}
si SWD
speed ${JLINK_SPEED}
connect
halt
w2 0x4001E3FA 0xA502
w1 0x4001EA90 0x00
w2 0x4001E3FA 0xA500
loadfile ${stripped}
r
g
q
EOF
  echo -e "${YELLOW}[local]${NC} flashing ${APP}..."
  JLinkExe -nogui 1 -SelectEmuBySN "${JLINK_SN}" -commanderscript "$jl" >"$log" 2>&1 || true
  if grep -qiE "Could not connect to the target device|RAMCode did not respond|Could not load|Could not find core in Coresight|could not be halted" "$log"; then
    echo -e "${RED}[local]${NC} J-Link flash error -- log tail:" >&2
    tail -20 "$log" >&2
    return 1
  fi
  if ! grep -qE "Programming flash.*Done\.|O\.K\.|Skipped\. Contents already match" "$log"; then
    echo -e "${RED}[local]${NC} flash never confirmed -- log tail:" >&2
    tail -20 "$log" >&2
    return 1
  fi
  echo -e "${YELLOW}[local]${NC} flash OK"
  return 0
}

# ---- Run a J-Link commander script, capture log -----------------------------
jlink_run() { # jlink_run <script-text> <logfile>
  local jl="/tmp/hil_local_${APP}_probe.jlink"
  printf '%s\n' "$1" >"$jl"
  JLinkExe -nogui 1 -SelectEmuBySN "${JLINK_SN}" -commanderscript "$jl" >"$2" 2>&1 || true
}

# =============================================================================
# Mode: uart_scrape
# =============================================================================
run_uart_scrape() {
  need_uart
  local timeout_s="${HIL_TIMEOUT_S:-30}"
  local log="/tmp/hil_local_${APP}.uart"
  echo -e "${YELLOW}[local]${NC} uart_scrape on ${UART}  expect='${HIL_EXPECT}'  timeout=${timeout_s}s"
  start_reader "$log" "$((timeout_s + 25))"
  if ! flash_local; then
    stop_reader
    return 1
  fi
  local result="TIMEOUT" deadline=$((SECONDS + timeout_s))
  while ((SECONDS < deadline)); do
    if grep -qaF "${HIL_EXPECT}" "$log" 2>/dev/null; then
      result="MATCH"
      break
    fi
    sleep 0.1
  done
  stop_reader
  echo "--- captured UART ---"
  LC_ALL=C tr -c '[:print:]\r\n\t' '.' <"$log" | sed 's/\r/\\r/g' | head -30 | sed 's/^/[uart] /' || true
  echo "--- end ---"
  if [[ -n "${HIL_EXPECT_NEGATIVE:-}" ]]; then
    local neg
    neg="$(grep -aiE -- "${HIL_EXPECT_NEGATIVE}" "$log" | head -3 || true)"
    if [[ -n "$neg" ]]; then
      echo -e "${RED}[local FAIL]${NC} ${APP}: matched HIL_EXPECT_NEGATIVE"
      while IFS= read -r l; do echo "    + $l"; done <<<"$neg"
      return 1
    fi
  fi
  if [[ "$result" == "MATCH" ]]; then
    echo -e "${GREEN}[local PASS]${NC} ${APP}: saw '${HIL_EXPECT}'"
    return 0
  fi
  echo -e "${RED}[local FAIL]${NC} ${APP}: '${HIL_EXPECT}' not seen within ${timeout_s}s"
  return 1
}

# =============================================================================
# Mode: jlink_memprobe
# =============================================================================
sym_addr() { # sym_addr <symbol> -> prints hex addr, or empty
  # Check the main ELF and, for two-project TrustZone apps, the Non-Secure
  # ELF (where the NS-resident bench globals live).
  local elfs=("$ELF")
  if [[ -f "${APP_DIR}/build/${APP}_ns.elf" ]]; then
    elfs+=("${APP_DIR}/build/${APP}_ns.elf")
  fi
  arm-none-eabi-nm --print-size "${elfs[@]}" |
    awk -v s="$1" '$NF==s && !f {print $1; f=1}'
}
run_memprobe() {
  local sym="${HIL_PROBE_SYMBOL:-}" min="${HIL_PROBE_MIN_ADVANCE:-4}" win="${HIL_PROBE_SECONDS:-3}"
  local fsym="${HIL_PROBE_FAILURE_SYMBOL:-}" fmax="${HIL_PROBE_MAX_FAILURE:-0}"
  # Boot dwell: let the app reach steady state before the FIRST read. Apps with
  # a slow bring-up (e.g. sd_font_render renders for ~6 s before its idle-loop
  # counter starts advancing) need this -- otherwise both reads straddle the
  # init phase (counter still 0, or stale SRAM from the prior image) and the
  # advance check is meaningless.
  local boot="${HIL_PROBE_BOOT_S:-0}"
  [[ -n "$sym" ]] || {
    echo -e "${RED}[local]${NC} memprobe needs HIL_PROBE_SYMBOL"
    return 2
  }
  local saddr faddr=""
  saddr="$(sym_addr "$sym")"
  [[ -n "$saddr" ]] || {
    echo -e "${RED}[local]${NC} symbol '${sym}' not in ${APP}.elf"
    return 1
  }
  [[ -n "$fsym" ]] && faddr="$(sym_addr "$fsym")"
  echo -e "${YELLOW}[local]${NC} memprobe symbol=${sym}@0x${saddr} min_advance=${min} window=${win}s boot_dwell=${boot}s${fsym:+ fail=${fsym}@0x${faddr}}"
  if ! flash_local; then return 1; fi
  local log="/tmp/hil_local_${APP}_memprobe.log"
  local script="device ${JLINK_DEVICE}
si SWD
speed ${JLINK_SPEED}
connect
halt"
  ((boot > 0)) && script+=$'\n'"go
sleep $((boot * 1000))
halt"
  script+=$'\n'"mem32 0x${saddr} 1"
  [[ -n "$faddr" ]] && script+=$'\n'"mem32 0x${faddr} 1"
  script+=$'\n'"go
sleep $((win * 1000))
halt
mem32 0x${saddr} 1"
  [[ -n "$faddr" ]] && script+=$'\n'"mem32 0x${faddr} 1"
  script+=$'\n'"q"
  jlink_run "$script" "$log"
  mapfile -t HITS < <(grep -iE "^${saddr}" "$log" | awk '{print $3}' | tr 'a-f' 'A-F')
  ((${#HITS[@]} >= 2)) || {
    echo -e "${RED}[local]${NC} could not read ${sym} twice"
    tail -20 "$log" >&2
    return 1
  }
  # Signed delta: a counter that goes BACKWARDS (post < pre) means the target
  # reset between the two reads -- not "steadily advancing" -- so it must fail,
  # not wrap around to a huge positive delta. These liveness counters never
  # legitimately wrap uint32 (4 billion ticks), so post < pre is always a reset.
  local pre=$((16#${HITS[0]})) post=$((16#${HITS[1]})) delta
  delta=$((post - pre))
  local fdelta=0 fpre="" fpost=""
  if [[ -n "$faddr" ]]; then
    mapfile -t FH < <(grep -iE "^${faddr}" "$log" | awk '{print $3}' | tr 'a-f' 'A-F')
    ((${#FH[@]} >= 2)) || {
      echo -e "${RED}[local]${NC} could not read ${fsym} twice"
      tail -20 "$log" >&2
      return 1
    }
    fpre="${FH[0]}"
    fpost="${FH[1]}"
    fdelta=$(((16#${FH[1]}) - (16#${FH[0]})))
  fi
  local ok=1
  ((delta < min)) && ok=0
  [[ -n "$faddr" ]] && ((fdelta > fmax)) && ok=0
  local msg="${sym}: 0x${HITS[0]} -> 0x${HITS[1]}, delta=${delta} (need >= ${min})"
  [[ -n "$faddr" ]] && msg+="; ${fsym}: 0x${fpre} -> 0x${fpost}, delta=${fdelta} (max ${fmax})"
  if ((ok == 1)); then
    echo -e "${GREEN}[local PASS]${NC} ${APP} (${msg})"
    return 0
  fi
  echo -e "${RED}[local FAIL]${NC} ${APP} (${msg})"
  echo "    (J-Link log: ${log})"
  return 1
}

# =============================================================================
# Mode: alive
# =============================================================================
run_alive() {
  need_uart
  local boot_s="${HIL_BOOT_S:-2}"
  echo -e "${YELLOW}[local]${NC} alive  boot_dwell=${boot_s}s"
  if ! flash_local; then return 1; fi
  local uart="/tmp/hil_local_${APP}.uart"
  start_reader "$uart" "$((boot_s + 3))" # outlive the dwell so stop_reader owns the kill
  sleep "$boot_s"
  stop_reader
  local log="/tmp/hil_local_${APP}_alive.log"
  local script="device ${JLINK_DEVICE}
si SWD
speed ${JLINK_SPEED}
connect
halt
mem32 0xE000ED28 1
mem32 0xE000ED2C 1
regs
go
sleep 1500
halt
regs
q"
  jlink_run "$script" "$log"
  local txt
  txt="$(cat "$log")"
  mapfile -t PCS < <(echo "$txt" | grep -oE 'PC[ ]*=[ ]*[0-9A-Fa-f]+' | grep -oE '[0-9A-Fa-f]+$' | tr 'a-f' 'A-F' | awk '!seen[$0]++')
  mapfile -t CYC < <(echo "$txt" | grep -oE 'CycleCnt[ ]*=[ ]*[0-9A-Fa-f]+' | grep -oE '[0-9A-Fa-f]+$' | tr 'a-f' 'A-F' | awk '!seen[$0]++')
  local pc1="${PCS[0]:-}" pc2="${PCS[$((${#PCS[@]} > 1 ? ${#PCS[@]} - 1 : 0))]:-${PCS[0]:-}}"
  local cyc1="${CYC[0]:-}" cyc2="${CYC[$((${#CYC[@]} > 1 ? ${#CYC[@]} - 1 : 0))]:-${CYC[0]:-}}"
  local cfsr hfsr
  cfsr="$(echo "$txt" | grep -iE '^E000ED28' | head -1 | awk '{print $3}' | tr 'a-f' 'A-F')"
  hfsr="$(echo "$txt" | grep -iE '^E000ED2C' | head -1 | awk '{print $3}' | tr 'a-f' 'A-F')"
  local ok=1 issues=()
  if [[ -z "$pc1" || -z "$cyc1" ]]; then
    issues+=("could not parse PC/CycleCnt")
    ok=0
  else
    local mram_lo=$((16#02000000)) mram_hi=$((16#02100000)) itcm_lo=0 itcm_hi=$((16#00010000))
    # dfu_bootloader copy-to-run target: a booted payload runs from the SRAM
    # window at k_ra8_dfu_run_base (0x22020000), so accept that as live code too
    # (real faults are still caught by the CFSR/HFSR + fault-spinner checks).
    local srun_lo=$((16#22020000)) srun_hi=$((16#22100000))
    in_code() {
      local p=$1
      { ((p >= mram_lo && p < mram_hi)) || ((p >= itcm_lo && p < itcm_hi)) || ((p >= srun_lo && p < srun_hi)); }
    }
    in_code $((16#$pc1)) || {
      issues+=("PC1=0x${pc1} outside MRAM/ITCM/SRAM-run")
      ok=0
    }
    in_code $((16#$pc2)) || {
      issues+=("PC2=0x${pc2} outside MRAM/ITCM/SRAM-run")
      ok=0
    }
    ((((16#$cyc2) - (16#$cyc1)) & 0xFFFFFFFF)) || {
      issues+=("CycleCnt frozen (0x${cyc1}->0x${cyc2})")
      ok=0
    }
  fi
  local spinner='(panic_halt|halt_loop|exception_halt|Default_Handler|HardFault_Handler|MemManage_Handler|BusFault_Handler|UsageFault_Handler|SecureFault_Handler|spin_forever|hang_forever|_die|__assert_fail)'
  if [[ -n "$pc1" ]] && command -v arm-none-eabi-addr2line >/dev/null 2>&1; then
    local s1 s2
    s1="$(arm-none-eabi-addr2line -e "$ELF" -f "0x${pc1}" 2>/dev/null | head -1)"
    s2="$(arm-none-eabi-addr2line -e "$ELF" -f "0x${pc2}" 2>/dev/null | head -1)"
    if echo "$s1" | grep -qE "$spinner" || echo "$s2" | grep -qE "$spinner"; then
      issues+=("PC in fault spinner: PC1=${s1:-?} PC2=${s2:-?}")
      ok=0
    fi
  fi
  if [[ -n "$cfsr" ]] && ((16#$cfsr != 0)); then
    issues+=("CFSR=0x${cfsr} non-zero")
    ok=0
  fi
  if [[ -n "$hfsr" ]] && (((16#$hfsr & ~(1 << 31)) != 0)); then
    issues+=("HFSR=0x${hfsr} real HardFault")
    ok=0
  fi
  if [[ -s "$uart" ]]; then
    local neg
    neg="$(grep -aiE '\b(FAIL|FAILED|panic|NAK|ERROR|HardFault|hardfault|MemFault|BusFault|UsageFault|stack[ _-]?overflow)\b' "$uart" | head -3 || true)"
    [[ -n "$neg" ]] && {
      issues+=("UART failure banner during boot:")
      while IFS= read -r l; do issues+=("      $l"); done <<<"$neg"
      ok=0
    }
  fi
  if ((ok == 1)); then
    echo -e "${GREEN}[local PASS]${NC} ${APP} (PC=0x${pc1}->0x${pc2}, CycleCnt 0x${cyc1}->0x${cyc2})"
    return 0
  fi
  echo -e "${RED}[local FAIL]${NC} ${APP}"
  for i in "${issues[@]}"; do echo "    - $i"; done
  echo "    (J-Link log: ${log})"
  return 1
}

case "$MODE" in
  uart_scrape) run_uart_scrape ;;
  jlink_memprobe) run_memprobe ;;
  alive) run_alive ;;
  *)
    echo -e "${RED}[local]${NC} unsupported HIL_MODE='${MODE}'"
    exit 2
    ;;
esac
