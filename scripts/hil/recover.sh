#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# hil_recover.sh -- EK-RA8D2 board recovery: flash even when things are broken.
#
# Root cause of most flash failures: the stock .hex includes OFS option-byte
# sections at 0x0300A100+.  J-Link's RAMCode times out when it tries to
# Prepare() that bank while the CPU is in a TrustZone-locked state.
# Fix: strip those sections before programming.
#
# What this script does:
#   1. Build the app if the .hex is missing.
#   2. Strip OFS sections from the hex (arm-none-eabi-objcopy).
#   3. Attempt to flash (halt + loadfile stripped hex + reset + go).
#   4. If flash fails, prompt for a manual power cycle then retry (up to
#      MAX_ATTEMPTS times total).
#   5. Optionally verify UART output.
#
# Usage:
#   bash scripts/hil/recover.sh <app>
#   bash scripts/hil/recover.sh uart_hello
#   bash scripts/hil/recover.sh uart_hello --expect "hello, ra8d2!"
#   bash scripts/hil/recover.sh uart_hello --uart /dev/cu.usbmodem001... --expect "hello"
#
# Exit codes:
#   0  -- flash succeeded (and UART matched if --expect given)
#   1  -- all attempts failed

set -uo pipefail

# Rig config (PI_HOST, JLINK_SN) comes from the gitignored .env, not the tree.
_hil_dir="$(dirname "${BASH_SOURCE[0]}")"
_hil_dir="$(cd "$_hil_dir" && pwd)"
# shellcheck source=scripts/hil/lib/rig_env.sh
source "$_hil_dir/lib/rig_env.sh"
rig_require PI_HOST JLINK_SN
MAX_ATTEMPTS=5
BAUD="115200"
TIMEOUT_S="10"
EXPECT=""
UART=""

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

tag() { printf "${CYAN}[recover]${NC} %s\n" "$*"; }
ok() { printf "${GREEN}[OK]${NC}  %s\n" "$*"; }
err() { printf "${RED}[FAIL]${NC} %s\n" "$*"; }
warn() { printf "${YELLOW}[WARN]${NC} %s\n" "$*"; }

[[ $# -ge 1 ]] || {
  echo "Usage: $0 <app> [--expect <string>] [--uart <dev>] [--baud N] [--timeout N]"
  exit 2
}
APP="$1"
shift

while [[ $# -gt 0 ]]; do
  case "$1" in
    --expect)
      EXPECT="$2"
      shift 2
      ;;
    --uart)
      UART="$2"
      shift 2
      ;;
    --baud)
      BAUD="$2"
      shift 2
      ;;
    --timeout)
      TIMEOUT_S="$2"
      shift 2
      ;;
    *)
      echo "Unknown arg: $1"
      exit 2
      ;;
  esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# ---- locate app dir ----------------------------------------------------------
APP_DIR="$(find "$ROOT/examples" -name "main.c" |
  sed 's|/main\.c||' |
  while read -r d; do
    [[ "$(basename "$d")" == "$APP" ]] && echo "$d" || true
  done | head -1)"

[[ -n "$APP_DIR" ]] || {
  err "app '$APP' not found under examples/"
  exit 1
}

HEX="${APP_DIR}/build/${APP}.hex"
ELF="${APP_DIR}/build/${APP}.elf"

# ---- build if needed ---------------------------------------------------------
if [[ ! -f "$HEX" ]]; then
  tag "building $APP..."
  make -C "$ROOT" "$APP"
fi
[[ -f "$HEX" ]] || {
  err "build failed -- no hex at $HEX"
  exit 1
}

# ---- bench mutual exclusion --------------------------------------------------
# Recovery is MORE destructive than an ordinary flash, so it is not exempt.
# Break-glass (RA8_BENCH_BREAK_GLASS="board wedged mid-flash") force-takes when
# the incumbent is dead or is the one that wedged the board -- journaled, and
# refused against a human holder without an explicit acknowledgement.
#
# The budget is generous because this loop power-cycles between attempts.
# shellcheck source=scripts/hil/lib/bench_lock.sh
source "$_hil_dir/lib/bench_lock.sh"
ra8_bench_require_recovery "recovery flash of ${APP}" 30m || exit $?

# ---- anti-recovery pre-flash guard ------------------------------------------
# Even a recovery flash must refuse an image that would brick recovery. Inspect
# the full pre-strip image + the source tree before any programming. See
# scripts/hil/lib/preflash_guard.sh.
# shellcheck source=scripts/hil/lib/preflash_guard.sh
source "$_hil_dir/lib/preflash_guard.sh"
ra8_preflash_guard "$HEX" || exit $?

# ---- strip OFS sections (the root cause of RAMCode timeout) ------------------
# OFS sections at 0x0300A100+ cause J-Link RAMCode to time out during
# Prepare() when TrustZone option bytes are involved.  Strip them so J-Link
# only programs the MRAM bank at 0x02000000 which works cleanly.
#
# SWD speed: RA8D2 boots from the internal ~4 MHz oscillator after SYSRESETREQ.
# J-Link's RAMCode runs at this low clock and requires speed <= 1000 kHz to
# communicate reliably.  Using 4000 kHz causes RAMCode timeout.
STRIPPED_HEX="/tmp/hil_recover_${APP}_mram.$$.hex"
OFS_SECTIONS=('--remove-section=.option_setting*')
if [[ -f "$ELF" ]]; then
  arm-none-eabi-objcopy "${OFS_SECTIONS[@]}" -O ihex "$ELF" "$STRIPPED_HEX" 2>/dev/null ||
    arm-none-eabi-objcopy -O ihex "$ELF" "$STRIPPED_HEX"
else
  arm-none-eabi-objcopy -I ihex "${OFS_SECTIONS[@]}" -O ihex "$HEX" "$STRIPPED_HEX" 2>/dev/null ||
    cp "$HEX" "$STRIPPED_HEX"
fi
tag "OFS-stripped hex: $STRIPPED_HEX"

# ---- flash attempt function --------------------------------------------------
internal_try_flash() {
  local remote_hex="/tmp/hil_recover_${APP}_mram.$$.hex"
  local log="/tmp/hil_recover_jlink_${APP}.$$.log"

  scp -q "$STRIPPED_HEX" "${PI_HOST}:${remote_hex}" 2>/dev/null || {
    printf '(cannot reach Pi %s)\n' "${PI_HOST}"
    return 1
  }

  # shellcheck disable=SC2087  # client-side substitution of JLINK_DEVICE/JLINK_SN/remote_hex is intentional
  ssh "$PI_HOST" bash <<REMOTE >"$log" 2>&1 || true
set -uo pipefail
TMP=\$(mktemp)
trap 'rm -f "\$TMP"' EXIT
cat > "\$TMP" <<JLINK
device ${JLINK_DEVICE}
si SWD
speed 1000
connect
halt
loadfile ${remote_hex}
r
g
q
JLINK
JLinkExe -nogui 1 -SelectEmuBySN ${JLINK_SN} -commanderscript "\$TMP" 2>&1
REMOTE

  local vtref halted ramcode_err ok_count download_count
  vtref="$(grep -oE 'VTref=[0-9.]+V' "$log" | head -1 | sed 's/VTref=//' || echo '?')"
  halted="$(grep -c 'CPU could not be halted' "$log" || true)"
  ramcode_err="$(grep -c 'RAMCode did not respond\|Failed to prepare' "$log" || true)"
  ok_count="$(grep -c 'O\.K\.' "$log" || true)"
  download_count="$(grep -c 'Downloading file\|Contents already match\|Skipped\.' "$log" || true)"

  printf "  VTref=%-8s " "$vtref"

  if [[ "$ok_count" -gt 0 && "$halted" -eq 0 && "$download_count" -gt 0 && "$ramcode_err" -eq 0 ]]; then
    return 0
  fi

  # show why it failed
  if grep -q 'Cannot connect to the probe' "$log" 2>/dev/null; then
    printf "(J-Link not found -- is USB connected to Pi?)\n"
  elif [[ "$halted" -gt 0 ]]; then
    printf "(CPU could not be halted)\n"
  elif [[ "$ramcode_err" -gt 0 ]]; then
    printf "(RAMCode timeout -- power cycle may help)\n"
    grep -E 'RAMCode|Failed to prepare' "$log" | head -3 | sed 's/^/    /'
  else
    printf "(unknown failure)\n"
    grep -iE 'error|fail' "$log" | grep -v 'Writing target memory' | head -3 | sed 's/^/    /'
  fi
  return 1
}

# ---- between-attempt power cycle ---------------------------------------------
# This used to be an unconditional `read -r` waiting for a person to press
# ENTER. A non-interactive caller -- the HIL suite, a CI job, an agent -- had no
# way to answer it, so the script sat there until the job timeout killed it,
# which reads as "recovery hung" rather than "recovery asked a question nobody
# could hear". MAX_ATTEMPTS=5 with an unbounded wait between each is not a
# bounded loop at all.
#
# So: drive the Tapo plug when there is no terminal, and only ask a human when
# there is one to ask. If neither is possible, fail fast and say why.
power_cycle_between_attempts() {
  warn "flash failed -- power-cycling the board"
  if [[ -t 0 ]]; then
    warn "(unplug and replug the USB cable, or hold the RESET button)"
    printf "  Press ENTER when board is back on, or wait to use the Tapo plug..."
    if read -r -t 30; then
      echo ""
      return 0
    fi
    echo ""
    warn "no answer in 30s -- using the Tapo plug instead"
  else
    warn "stdin is not a terminal, so nobody can press ENTER -- using the Tapo plug"
  fi
  # We already hold the bench, so tapo.sh's own guard is a no-op here.
  if bash "$_hil_dir/tapo.sh" board cycle; then
    tag "board plug cycled -- waiting 5s for enumeration"
    sleep 5
    return 0
  fi
  err "cannot power-cycle the board: no terminal to ask, and the Tapo plug"
  err "did not respond. Fix the plug (make hil-tapo TARGET=board CMD=status)"
  err "or re-run this from a terminal."
  return 1
}

# ---- flash loop --------------------------------------------------------------
tag "flashing $APP (up to $MAX_ATTEMPTS attempts)..."
attempt=0
while [[ $attempt -lt $MAX_ATTEMPTS ]]; do
  attempt=$((attempt + 1))
  printf "  [attempt %d/%d] " "$attempt" "$MAX_ATTEMPTS"

  if internal_try_flash; then
    printf "\n"
    ok "flashed $APP on attempt $attempt"
    break
  fi

  if [[ $attempt -lt $MAX_ATTEMPTS ]]; then
    echo ""
    power_cycle_between_attempts || exit 1
  fi
done

if [[ $attempt -ge $MAX_ATTEMPTS ]]; then
  # Check if the last attempt succeeded (loop exits on break or exhaustion)
  printf "  [attempt %d/%d] " "$((attempt))" "$MAX_ATTEMPTS"
  if internal_try_flash; then
    printf "\n"
    ok "flashed $APP on attempt $attempt"
  else
    printf "\n"
    err "all $MAX_ATTEMPTS attempts failed -- see /tmp/hil_recover_jlink_${APP}.$$.log on Pi"
    exit 1
  fi
fi

# ---- UART verification (runs on Pi) ------------------------------------------
if [[ -z "$EXPECT" ]]; then
  exit 0
fi

# Resolve the console on the Pi by device identity when none was named. The
# old default was /dev/ttyACM0, which after a re-enumeration is as likely to be
# the ESP32-C6's UART bridge as the board -- and a recovery run that reads the
# wrong device reports a dead board that is actually fine.
if [[ -z "$UART" ]]; then
  UART=$(
    # shellcheck disable=SC2087  # RA8_TTY_RESOLVER_SRC/JLINK_SN are substituted client-side on purpose: the Pi has no checkout.
    ssh "$PI_HOST" bash -s <<REMOTE
${RA8_TTY_RESOLVER_SRC}
JLINK_SN="${JLINK_SN}"
ra8_tty_resolve console
REMOTE
  ) || {
    err "could not resolve the board console on ${PI_HOST}"
    exit 1
  }
fi

tag "waiting for '$EXPECT' on Pi:$UART (${TIMEOUT_S}s)..."

RESULT=$(
  # shellcheck disable=SC2087  # client-side substitution of UART/BAUD/TIMEOUT_S/EXPECT is intentional
  ssh "$PI_HOST" bash <<REMOTE
set -euo pipefail
stty -F ${UART} ${BAUD} raw -echo cs8 -cstopb -parenb
timeout ${TIMEOUT_S} bash -c '
    while IFS= read -r line <&3; do
        echo "[uart] \$line"
        if echo "\$line" | grep -qF "${EXPECT}"; then
            echo "MATCH"
            exit 0
        fi
    done
    exit 1
' 3<>${UART} && echo "FOUND" || echo "TIMEOUT"
REMOTE
)

echo "$RESULT" | grep '^\[uart\]' | head -10

if echo "$RESULT" | grep -q "FOUND\|MATCH"; then
  ok "$APP: saw '$EXPECT'"
  exit 0
else
  err "$APP: '$EXPECT' not seen within ${TIMEOUT_S}s"
  exit 1
fi
