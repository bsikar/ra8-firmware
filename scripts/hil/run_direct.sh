#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# hil_run_direct.sh -- Flash a firmware .hex and verify expected UART output.
#
# Auto-detects environment:
#  * If running ON the HIL Pi (see rig_is_local_pi), runs JLinkExe and reads
#    the board console directly. The console is resolved by device identity
#    (scripts/hil/lib/tty_resolve.sh), never by ttyACM number.
#  * If running on a developer workstation, SCPs the hex to the Pi, SSHes
#    in, and re-invokes itself there. The shape of the test (expect string,
#    timeout) is preserved unchanged.
#
# Usage (works from either side):
#   scripts/hil/run_direct.sh --hex <path/to/app.hex> \
#                             --expect <string>            \
#                             [--expect-negative <regex>]  \
#                             [--baud 115200]              \
#                             [--timeout 10]               \
#                             [--uart <device>]
#
# --expect-negative <regex>
#     Extended-regex (grep -E) of substrings that, if seen in the UART
#     log, fail the run -- even when the positive --expect matched.
#     Closes the class of weak hil.conf where HIL_EXPECT="fxlx"
#     matches both "fxlx: booting" and "fxlx: lx_nor_flash_format
#     failed". Per-app hil.conf passes this in as HIL_EXPECT_NEGATIVE.
#
# Sanity gates run on the inputs themselves (rejected with exit 2
# before flashing):
#   - --expect must be >= 12 chars unless HIL_EXPECT_SHORT_OK=1 in the
#     environment (small-app escape hatch documented in the README).
#   - --expect must not be a substring of any failure-banner string in
#     the .elf .rodata (where "failure banner" means a string matching
#     /FAIL|panic|NAK|ERROR|failed/ via arm-none-eabi-strings).
#
# Exit codes:
#   0  PASS  -- positive expect matched within timeout AND no negative
#               expect (if set) matched in the captured log
#   1  FAIL  -- timeout elapsed without positive match, OR negative
#               expect matched, OR flash failed
#   2  ERROR -- missing arguments, hardware not reachable, or sanity
#               gates rejected the input

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
  echo "Usage: $0 --hex <file> --expect <string> [--expect-negative <regex>] [--baud 115200] [--timeout 10] [--uart <device>]"
  exit 2
}

HEX=""
EXPECT=""
EXPECT_NEG="${HIL_EXPECT_NEGATIVE:-}"
BAUD="115200"
TIMEOUT_S="10"
# Empty on purpose: the console is resolved by identity below, and there is no
# ttyACM number that is right often enough to be a default.
UART=""
UART_EXPLICIT=""

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
    --expect-negative)
      EXPECT_NEG="$2"
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
      UART_EXPLICIT=1
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
  echo -e "${RED}[HIL]${NC} hex not found: $HEX"
  exit 2
}

# Sanity gate 1: minimum positive-expect length.
# "fxlx" (4 chars) is the canonical failure case -- it appears in both
# "fxlx: booting..." (success) and "fxlx: lx_nor_flash_format failed"
# (failure). 12 chars effectively requires a colon-banner-with-words
# pattern that can't accidentally overlap with a failure path.
MIN_EXPECT_LEN=12
if [[ "${HIL_EXPECT_SHORT_OK:-0}" != "1" && ${#EXPECT} -lt $MIN_EXPECT_LEN ]]; then
  echo -e "${RED}[HIL]${NC} --expect='${EXPECT}' is too short (${#EXPECT} < ${MIN_EXPECT_LEN} chars)."
  echo "    Tighten the banner so it can't accidentally match a failure path."
  echo "    Override (not recommended): HIL_EXPECT_SHORT_OK=1"
  exit 2
fi

# Sanity gate 2: overlap with .elf failure banners.
# Pull every string >= len(EXPECT) from the matching .elf via
# arm-none-eabi-strings; if any string that contains EXPECT also
# contains a failure word, the EXPECT pattern can match the
# failure banner -- reject before flashing.
ELF_FOR_STRINGS="${HEX%.hex}.elf"
if [[ -f "$ELF_FOR_STRINGS" ]] && command -v arm-none-eabi-strings >/dev/null 2>&1; then
  OVERLAP_HITS=$(arm-none-eabi-strings -n "${MIN_EXPECT_LEN}" "$ELF_FOR_STRINGS" 2>/dev/null |
    grep -F -- "$EXPECT" |
    grep -iE '\b(FAIL|FAILED|panic|NAK|ERROR|failed|HardFault|MemFault|BusFault|UsageFault|abort)\b' |
    head -3 || true)
  if [[ -n "$OVERLAP_HITS" ]]; then
    echo -e "${RED}[HIL]${NC} --expect='${EXPECT}' overlaps failure banners in $(basename "$ELF_FOR_STRINGS"):"
    while IFS= read -r line; do echo "    + $line"; done <<<"$OVERLAP_HITS"
    echo "    Pick a HIL_EXPECT that is unique to the success path."
    echo "    Override (not recommended): HIL_EXPECT_OVERLAP_OK=1"
    if [[ "${HIL_EXPECT_OVERLAP_OK:-0}" != "1" ]]; then
      exit 2
    fi
    echo -e "${YELLOW}[HIL]${NC} HIL_EXPECT_OVERLAP_OK=1 set -- continuing despite overlap."
  fi
fi

APP_NAME="$(basename "${HEX%.hex}")"
LOG_FILE="/tmp/hil_jlink_${APP_NAME}.log"

# Detect whether we're already on the Pi (matches the pattern used by
# hil_usb_test.sh: hostname OR aarch64 with a CDC device attached).
LOCAL_PI=0
if rig_is_local_pi; then
  LOCAL_PI=1
fi

# Off-Pi: scp the hex over, re-invoke ourselves on the Pi with --hex
# pointing at the remote copy. This way every Pi-local step (JLinkExe,
# console reads) runs natively, and the developer just sees the
# pass/fail on stdout.
if ((LOCAL_PI == 0)); then
  ssh -o ConnectTimeout=5 -o BatchMode=yes "$PI_HOST" true 2>/dev/null ||
    {
      echo -e "${RED}[HIL]${NC} cannot reach ${PI_HOST}"
      exit 2
    }
  REMOTE_HEX="/tmp/$(basename "$HEX")"
  scp -q "$HEX" "${PI_HOST}:${REMOTE_HEX}"
  # If an ELF sibling exists, copy it too -- the OFS-strip path uses it.
  if [[ -f "${HEX%.hex}.elf" ]]; then
    scp -q "${HEX%.hex}.elf" "${PI_HOST}:${REMOTE_HEX%.hex}.elf"
  fi
  # Re-invoke ourselves on the Pi. Quote the expect string carefully so
  # spaces / brackets survive the SSH transport.
  REMOTE_ARGS="--hex '${REMOTE_HEX}' --expect $(printf '%q' "$EXPECT")"
  if [[ -n "$EXPECT_NEG" ]]; then
    REMOTE_ARGS+=" --expect-negative $(printf '%q' "$EXPECT_NEG")"
  fi
  REMOTE_ARGS+=" --baud '${BAUD}' --timeout '${TIMEOUT_S}'"
  # Only forward --uart when the caller actually named one. Forwarding an
  # empty value would pin the remote side to nothing; without it the Pi
  # resolves its own console by identity, which is what it should do anyway.
  if [[ -n "$UART_EXPLICIT" ]]; then
    REMOTE_ARGS+=" --uart '${UART}'"
  fi
  # Propagate the sanity-gate escape-hatch flags so the remote side
  # doesn't re-reject inputs the local side already vetted.
  REMOTE_ENV=""
  [[ "${HIL_EXPECT_SHORT_OK:-0}" == "1" ]] && REMOTE_ENV+="HIL_EXPECT_SHORT_OK=1 "
  [[ "${HIL_EXPECT_OVERLAP_OK:-0}" == "1" ]] && REMOTE_ENV+="HIL_EXPECT_OVERLAP_OK=1 "
  # shellcheck disable=SC2029  # ${REMOTE_ENV}/${REMOTE_ARGS} are composed locally for the piped copy of this script.
  ssh "$PI_HOST" "${REMOTE_ENV}bash -s -- ${REMOTE_ARGS}" <"$0"
  exit $?
fi

echo -e "${YELLOW}[HIL]${NC} app=${APP_NAME}  expect='${EXPECT}'  timeout=${TIMEOUT_S}s"

# ---- 1. Strip OFS sections ---------------------------------------------------
# OFS sections at 0x0300A100+ cause J-Link RAMCode to timeout during Prepare()
# when TrustZone option bytes are involved.  Strip them so J-Link only programs
# the MRAM bank at 0x02000000.
ELF="${HEX%.hex}.elf"
STRIPPED_HEX="/tmp/hil_${APP_NAME}_mram.hex"
OFS_ARGS=('--remove-section=.option_setting*')
if [[ -f "$ELF" ]]; then
  arm-none-eabi-objcopy "${OFS_ARGS[@]}" -O ihex "$ELF" "$STRIPPED_HEX" 2>/dev/null ||
    cp "$HEX" "$STRIPPED_HEX"
else
  arm-none-eabi-objcopy -I ihex "${OFS_ARGS[@]}" -O ihex "$HEX" "$STRIPPED_HEX" 2>/dev/null ||
    cp "$HEX" "$STRIPPED_HEX"
fi

# ---- 2. Flash via J-Link (loadfile with OFS-stripped hex) -------------------
# Direct w4 writes don't commit to MRAM cells without the MRMS flush sequence
# (MRCPC1 gate + MRCFLR flush per HUM Ch 59).  Implementing that in J-Link
# Commander is impractical, so we use loadfile (which uses RAMCode internally).
# OFS stripping above prevents the RAMCode Prepare() timeout that occurs when
# .option_setting* sections at 0x0300A100 are included.
#
# Pre-flash LPSCR clear (HUM Ch 11.2.18 / 11.2.20):
# Some apps (e.g. power_profiler) write LPSCR.LPMD = 0x4 (software standby)
# before WFI.  SYSRESETREQ via the debugger does NOT reset the SYSC LPM block
# (separate reset domain), so LPSCR survives reset.  When J-Link's RAMCode
# helper later executes any WFI, it gets trapped into software standby with
# no wake source -- "RAMCode did not respond" timeout cascades.  Clearing
# LPSCR via DAP (PRCR-unlock + write 0 + relock) makes RAMCode's WFI a plain
# Sleep that any interrupt can wake.
TMP_SCRIPT="$(mktemp)"
# Single-quoted body: ${UART} is read at trap time, not now. It is still empty
# here (the console is resolved after flashing), and a pkill pattern of a bare
# "cat " would match far more than this script's reader.
# shellcheck disable=SC2016  # deliberate late expansion of $UART, see above.
trap 'rm -f "$TMP_SCRIPT" "$STRIPPED_HEX"; [ -n "$UART" ] && pkill -f "cat $UART" 2>/dev/null; true' EXIT
cat >"$TMP_SCRIPT" <<JLINK
device ${JLINK_DEVICE}
si SWD
speed 1000
connect
halt
w2 0x4001E3FA 0xA502
w1 0x4001EA90 0x00
w2 0x4001E3FA 0xA500
loadfile ${STRIPPED_HEX}
r
g
q
JLINK

echo -e "${YELLOW}[HIL]${NC} flashing ${HEX}..."

# Start UART reader in the background BEFORE flashing.  The firmware's boot
# banner prints within milliseconds of "g" (go) -- if we open the console
# only after JLinkExe returns, one-shot boot banners are missed because the
# bytes arrive before any reader is attached.  Configure the tty first, then
# launch a background tail that streams to a log file we can grep afterward.
#
# Resolve the console by device identity, with a retry window that covers the
# J-Link briefly disappearing between back-to-back flash cycles. There is no
# ttyACM number to default to: the chip's own USBHS CDC (1209:xxxx) and the
# ESP32-C6's UART bridge enumerate in that namespace too, and which number
# each takes depends on plug order. See scripts/hil/lib/tty_resolve.sh.
if [ -z "$UART_EXPLICIT" ]; then
  UART="$(ra8_tty_resolve console)" || exit 1
fi
echo -e "${YELLOW}[HIL]${NC} board console: ${UART}"

# Kill any lingering readers from a previous test that didn't clean up.
# Two cats on the same console each get only half the bytes, which silently
# breaks pattern matching for the next test.
pkill -f "cat ${UART}" 2>/dev/null || true
sleep 0.1
stty -F "${UART}" "${BAUD}" raw -echo
UART_LOG="/tmp/hil_uart_${APP_NAME}.log"
: >"${UART_LOG}"
# Drain any stale bytes from the previous test that are still queued in
# the kernel-side tty receive buffer (`stty` doesn't tcflush). Without
# this, the new firmware's output is preceded by the previous app's
# output and the head -20 display truncates the new bytes off-screen.
# A short non-blocking read + discard is enough; stale buffers are
# typically a few hundred bytes.
dd if="${UART}" iflag=nonblock of=/dev/null count=4 bs=1024 2>/dev/null || true
# Use setsid so the cat does not share our session/process group -- this
# makes the cleanup pkill at end-of-script reliable regardless of exit path.
# stdbuf -o0 disables stdout buffering so every byte received from the tty
# is written to the log file immediately.  Without it, one-shot boot
# banners (e.g. "ulpt: wake\r\n" = 12 bytes) sit in cat's 4KB output buffer
# and grep races find an empty log.
setsid stdbuf -o0 cat "${UART}" >"${UART_LOG}" 2>/dev/null &
READER_PID=$!
# Make sure the reader actually opened the tty before we proceed.
sleep 0.2

# Single attempt: each loadfile op accumulates state in the MRAM controller
# (~13-op limit before PORST is required).  Retries make the accumulation
# worse without recovering from it, so we fail fast and let the suite move
# on; the user can power-cycle and rerun any failed apps.
JLinkExe -nogui 1 -SelectEmuBySN "${JLINK_SN}" -commanderscript "$TMP_SCRIPT" \
  >"${LOG_FILE}" 2>&1

if grep -qE "\*\*\*\*\*\* Error|Cannot connect to the probe|could not be halted|RAMCode did not respond" "${LOG_FILE}"; then
  kill "${READER_PID}" 2>/dev/null
  echo -e "${RED}[HIL]${NC} J-Link error -- log tail:" >&2
  tail -20 "${LOG_FILE}" >&2
  exit 1
fi
if ! grep -qE "Programming flash.*Done\.|Skipped\. Contents already match" "${LOG_FILE}"; then
  kill "${READER_PID}" 2>/dev/null
  echo -e "${RED}[HIL]${NC} flash phase missing -- log tail:" >&2
  tail -20 "${LOG_FILE}" >&2
  exit 1
fi
echo -e "${YELLOW}[HIL]${NC} flash OK"

# ---- 4. Wait for the expected string on UART  -----------------------------
# The background reader started before flashing has been capturing into
# ${UART_LOG} the whole time; tail-follow it until we see EXPECT or timeout.
echo -e "${YELLOW}[HIL]${NC} waiting for '${EXPECT}' on ${UART} (${TIMEOUT_S}s)..."

RESULT="TIMEOUT"
# Poll the log file every 100 ms for up to TIMEOUT_S seconds.  This is
# more robust than `tail -F | grep` which had a race where small one-shot
# prints (12-26 bytes) sat in cat's stdio buffer and were not visible to
# grep until cat was killed and flushed.
deadline=$((SECONDS + TIMEOUT_S))
while ((SECONDS < deadline)); do
  if grep -qF "${EXPECT}" "${UART_LOG}" 2>/dev/null; then
    RESULT="MATCH"
    break
  fi
  sleep 0.1
done
echo "--- captured UART ---"
sed 's/\r/\\r/g' "${UART_LOG}" | head -20 | sed 's/^/[uart] /'
echo "--- end ---"

# Stop the background tty reader.  It will keep running otherwise, consuming
# data from the console and breaking the next test's reader.  setsid moved
# it out of our process group, so we pkill by name too as a safety net.
kill "${READER_PID}" 2>/dev/null || true
pkill -f "cat ${UART}" 2>/dev/null || true
wait "${READER_PID}" 2>/dev/null || true

# Negative-expect scan -- runs even on positive match. The point is
# to catch firmware that emits BOTH a healthy boot banner and an
# error banner later in the same run; a positive-only test would
# wrongly pass.
if [[ -n "$EXPECT_NEG" ]]; then
  NEG_HIT=$(grep -iE -- "$EXPECT_NEG" "${UART_LOG}" | head -3 || true)
  if [[ -n "$NEG_HIT" ]]; then
    echo -e "${RED}[HIL FAIL]${NC} ${APP_NAME}: matched --expect-negative='${EXPECT_NEG}'"
    while IFS= read -r line; do echo "    + $line"; done <<<"$NEG_HIT"
    exit 1
  fi
fi

if [[ "${RESULT}" == "MATCH" ]]; then
  echo -e "${GREEN}[HIL PASS]${NC} ${APP_NAME}: saw '${EXPECT}'"
  exit 0
else
  echo -e "${RED}[HIL FAIL]${NC} ${APP_NAME}: '${EXPECT}' not seen within ${TIMEOUT_S}s"
  exit 1
fi
