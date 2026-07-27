#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# hil_run.sh -- Flash a firmware image (.elf or .hex) to the EK-RA8D2 via
# the Pi and verify expected output appears on the J-Link OB UART within a
# timeout.
#
# The Pi is the HIL host: the EK-RA8D2 board is physically wired to it, and its
# console (the J-Link OB VCOM carrying SCI8 at 115200 baud) is resolved by
# device identity through scripts/hil/lib/tty_resolve.sh -- never by ttyACM
# number, which is assignment order and changes on a power cycle.
#
# Usage (run from the repo root on the dev machine):
#   scripts/hil/run.sh --hex <path/to/app.elf|app.hex> \
#                      --expect <string>                \
#                      [--baud 115200]                  \
#                      [--timeout 10]                   \
#                      [--uart /dev/serial/by-id/...]
#
# Exit codes:
#   0  PASS  -- expected string appeared within timeout
#   1  FAIL  -- timeout elapsed without match, or flash failed
#   2  ERROR -- missing arguments / unreachable Pi

set -euo pipefail

# Rig config (PI_HOST, JLINK_SN) comes from the gitignored .env, not the tree.
_hil_dir="$(dirname "${BASH_SOURCE[0]}")"
_hil_dir="$(cd "$_hil_dir" && pwd)"
# shellcheck source=scripts/hil/lib/rig_env.sh
source "$_hil_dir/lib/rig_env.sh"
rig_require PI_HOST JLINK_SN
JLINK_DEVICE="R7KA8D2KF_CPU0"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

usage() {
  echo "Usage: $0 --hex <file.elf|file.hex> --expect <string> [--baud 115200] [--timeout 10] [--uart <device>]"
  exit 2
}

HEX=""
EXPECT=""
BAUD="115200"
TIMEOUT_S="10"
UART=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --hex)
      HEX="$2"
      shift 2
      ;;
    --expect)
      EXPECT="$2"
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
    --uart)
      UART="$2"
      shift 2
      ;;
    *)
      echo "Unknown arg: $1"
      usage
      ;;
  esac
done

[[ -z "$HEX" || -z "$EXPECT" ]] && usage
[[ -f "$HEX" ]] || {
  echo -e "${RED}[HIL]${NC} firmware file not found: $HEX"
  exit 2
}

# Resolve the board console on the Pi when --uart was not given. Identity, not
# number: the chip's own USBHS CDC and the ESP32-C6's UART bridge both
# enumerate as /dev/ttyACM<n> too, and which number each gets depends on the
# order they were plugged in. See scripts/hil/lib/tty_resolve.sh.
if [[ -z "$UART" ]]; then
  UART=$(
    # shellcheck disable=SC2087  # RA8_TTY_RESOLVER_SRC/JLINK_SN are substituted client-side on purpose: the Pi has no checkout.
    ssh "$PI_HOST" bash -s <<REMOTE
${RA8_TTY_RESOLVER_SRC}
JLINK_SN="${JLINK_SN}"
ra8_tty_resolve console
REMOTE
  ) || {
    echo -e "${RED}[HIL]${NC} could not resolve the board console on ${PI_HOST}" >&2
    exit 2
  }
fi

APP_NAME="$(basename "${HEX%.*}")"
FIRMWARE_EXT="${HEX##*.}"
REMOTE_FW="/tmp/hil_${APP_NAME}.${FIRMWARE_EXT}"

echo -e "${YELLOW}[HIL]${NC} app=${APP_NAME}  expect='${EXPECT}'  timeout=${TIMEOUT_S}s"

# ---- 0. Anti-recovery pre-flash guard ----------------------------------------
# Inspect the full image + source tree before any programming. See
# scripts/hil/lib/preflash_guard.sh.
# shellcheck source=scripts/hil/lib/preflash_guard.sh
source "$_hil_dir/lib/preflash_guard.sh"
ra8_preflash_guard "$HEX" || exit $?

# ---- 1. Option-setting (OFS/OTP) sections: strip by default (brick-safety) ---
# The .option_setting_* sections carry the RA8D2 option bytes: OFS0..3, SAS, BPS
# and the OTP structures (FSBL, PBPS, POFSPS, REVOKE, HUK-zeroize enable,
# anti-rollback) at their real HUM Ch 7 addresses (0x02C9_F0xx / 0x02E0_7600..).
# SEVERAL ARE ONE-TIME-PROGRAMMABLE: a wrong value can permanently lock or brick
# the part, and a bad OFS also stalls J-Link's RAMCode during Prepare(). The
# default flashing policy is therefore DELIBERATE, not incidental -- strip every
# .option_setting section and program only the code-MRAM bank at 0x02000000.
#
# There is a separate, explicit path for provisioning the option bytes on
# purpose: set RA8_HIL_PROGRAM_OPTION_BYTES=1 and the sections are flashed as-is.
# Only do that with values you have vetted against HUM Ch 7 -- an erased
# (all-ones) word is the permissive default and safe; anything else is on you.
#
# SWD speed: RA8D2 boots from the internal ~4 MHz oscillator after SYSRESETREQ.
# J-Link's RAMCode runs at this low clock and requires speed <= 1000 kHz to
# communicate reliably.  Using 4000 kHz causes RAMCode timeout.
ELF="${HEX%.hex}.elf"
STRIPPED_FW="/tmp/hil_${APP_NAME}_mram.${FIRMWARE_EXT}"
if [[ "${RA8_HIL_PROGRAM_OPTION_BYTES:-0}" == "1" ]]; then
  OFS_ARGS=()
  echo -e "${YELLOW}[HIL]${NC} RA8_HIL_PROGRAM_OPTION_BYTES=1 -- flashing option bytes (NOT stripped)"
else
  OFS_ARGS=('--remove-section=.option_setting*')
fi
if [[ "$FIRMWARE_EXT" == "elf" ]]; then
  arm-none-eabi-objcopy "${OFS_ARGS[@]}" -O ihex "$HEX" "/tmp/hil_${APP_NAME}_mram.hex" 2>/dev/null ||
    arm-none-eabi-objcopy -O ihex "$HEX" "/tmp/hil_${APP_NAME}_mram.hex"
  STRIPPED_FW="/tmp/hil_${APP_NAME}_mram.hex"
elif [[ -f "$ELF" ]]; then
  arm-none-eabi-objcopy "${OFS_ARGS[@]}" -O ihex "$ELF" "$STRIPPED_FW" 2>/dev/null ||
    cp "$HEX" "$STRIPPED_FW"
else
  arm-none-eabi-objcopy -I ihex "${OFS_ARGS[@]}" -O ihex "$HEX" "$STRIPPED_FW" 2>/dev/null ||
    cp "$HEX" "$STRIPPED_FW"
fi

# ---- 2. Copy firmware to Pi --------------------------------------------------
REMOTE_FW="/tmp/hil_${APP_NAME}_mram.hex"
echo -e "${YELLOW}[HIL]${NC} uploading hex..."
scp -q "$STRIPPED_FW" "${PI_HOST}:${REMOTE_FW}"

# ---- 3. Flash, then read UART for the expected string ------------------------
# ORDERING (issue #390): the UART reader MUST be live BEFORE the core is
# released from reset. A "print-once" app emits its banner within a few
# milliseconds of "g" (go) and then parks; if the tty is opened only AFTER
# JLinkExe returns, those bytes are already gone and the app reads as a silent
# hang -- indistinguishable from a genuine lock-up. So, on the Pi, we (1) start
# a background reader capturing the tty into a log, (2) let it settle so the
# port is open, (3) run JLinkExe (which loadfiles, resets, and goes), and only
# then (4) poll the log for EXPECT within the timeout. The reader is therefore
# already draining the port at the instant "g" runs, so the first post-reset
# bytes are caught. This mirrors the silicon-proven order in
# scripts/hil/run_direct.sh (reader-before-reset), which the suite already uses.
echo -e "${YELLOW}[HIL]${NC} flashing + capturing '${EXPECT}' on ${UART} (${TIMEOUT_S}s)..."
RESULT=$(
  # shellcheck disable=SC2087  # client-side substitution of JLINK_DEVICE/JLINK_SN/REMOTE_FW/APP_NAME/UART/BAUD/EXPECT/TIMEOUT_S is intentional
  ssh "$PI_HOST" bash <<REMOTE
set -euo pipefail
LOG=/tmp/hil_jlink_${APP_NAME}.log
UART_LOG=/tmp/hil_uart_${APP_NAME}.log
TMP_SCRIPT=\$(mktemp)
trap 'rm -f "\$TMP_SCRIPT"' EXIT
cat > "\$TMP_SCRIPT" <<JLINK
device ${JLINK_DEVICE}
si SWD
speed 1000
connect
halt
loadfile ${REMOTE_FW}
r
g
q
JLINK

# 1. Start the UART reader BEFORE JLinkExe resets and runs the target. setsid
#    detaches it from our process group so the end-of-run pkill is reliable;
#    stdbuf -o0 flushes every byte to the log immediately so a short one-shot
#    banner is visible to grep without waiting for cat's stdio buffer to fill.
stty -F ${UART} ${BAUD} raw -echo cs8 -cstopb -parenb
: > "\$UART_LOG"
setsid stdbuf -o0 cat ${UART} > "\$UART_LOG" 2>/dev/null &
READER_PID=\$!
# 2. Let the reader open the tty before the core is released from reset.
sleep 0.2

# 3. Flash + reset + go. The boot banner now lands in the live capture.
JLinkExe -nogui 1 -SelectEmuBySN ${JLINK_SN} -commanderscript "\$TMP_SCRIPT" > "\$LOG" 2>&1 || true
if grep -qiE "\*\*\*\*\*\* Error|^Error|could not load|RAMCode did not respond|could not be halted|Cannot connect" "\$LOG"; then
    kill "\$READER_PID" 2>/dev/null || true
    pkill -f "cat ${UART}" 2>/dev/null || true
    echo "J-Link error log:" >&2
    cat "\$LOG" >&2
    echo "RESULT=FLASHFAIL"
    exit 0
fi
if ! grep -q "O\.K\." "\$LOG"; then
    kill "\$READER_PID" 2>/dev/null || true
    pkill -f "cat ${UART}" 2>/dev/null || true
    echo "J-Link did not confirm download (no 'O.K.' in log):" >&2
    cat "\$LOG" >&2
    echo "RESULT=FLASHFAIL"
    exit 0
fi

# 4. The reader has been capturing since before "g"; poll the log for EXPECT.
#    Polling a file (not a live pipe) sidesteps the stdio-buffer race and keeps
#    the ${TIMEOUT_S} wait window as the post-flash budget, exactly as before.
FOUND=TIMEOUT
deadline=\$((SECONDS + ${TIMEOUT_S}))
while ((SECONDS < deadline)); do
    if grep -qF "${EXPECT}" "\$UART_LOG" 2>/dev/null; then
        FOUND=MATCH
        break
    fi
    sleep 0.1
done

# Stop the reader so it does not keep draining the tty into the next run.
kill "\$READER_PID" 2>/dev/null || true
pkill -f "cat ${UART}" 2>/dev/null || true
head -n 20 "\$UART_LOG" | sed 's/^/[uart] /' || true
echo "RESULT=\$FOUND"
REMOTE
)

echo -e "${YELLOW}[HIL]${NC} captured output:"
printf '%s\n' "$RESULT"

if echo "$RESULT" | grep -q "RESULT=FLASHFAIL"; then
  echo -e "${RED}[HIL FAIL]${NC} ${APP_NAME}: flash failed"
  exit 1
elif echo "$RESULT" | grep -q "RESULT=MATCH"; then
  echo -e "${GREEN}[HIL PASS]${NC} ${APP_NAME}: saw '${EXPECT}'"
  exit 0
else
  echo -e "${RED}[HIL FAIL]${NC} ${APP_NAME}: '${EXPECT}' not seen within ${TIMEOUT_S}s"
  exit 1
fi
