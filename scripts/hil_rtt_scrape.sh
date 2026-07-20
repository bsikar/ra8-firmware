#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# hil_rtt_scrape.sh -- flash a firmware .hex, let it run, then read its
# SEGGER RTT up-buffer directly via J-Link `mem` reads (avoids
# JLinkRTTLogger's reset-on-connect quirk). Mirrors hil_run_direct.sh's
# CLI so the batch HIL runner can dispatch to it the same way.
#
# Usage:
#   scripts/hil_rtt_scrape.sh --hex <path/to/app.hex>      \
#                             --elf <path/to/app.elf>      \
#                             --expect <string>            \
#                             [--rtt-buf-symbol s_rtt_up_buf] \
#                             [--rtt-buf-bytes 1024]       \
#                             [--expect-negative <regex>]  \
#                             [--timeout 10]

set -euo pipefail

# Rig config (PI_HOST) comes from the gitignored .env, not the tree.
_hil_dir="$(dirname "${BASH_SOURCE[0]}")"
_hil_dir="$(cd "$_hil_dir" && pwd)"
# shellcheck source=scripts/lib/rig_env.sh
source "$_hil_dir/lib/rig_env.sh"
rig_require PI_HOST

# Detect when we are already running on the Pi itself (self-hosted CI
# runner, or running locally on the bench). In that case skip the
# `ssh $PI_HOST` round-trip -- mDNS resolution of the Pi's `.local` name can fail
# inside a self-hosted runner's network namespace and we have direct
# access to the same files / J-Link / TTY here.
LOCAL_PI=0
if rig_is_local_pi; then
  LOCAL_PI=1
fi
pi_run() {
  if ((LOCAL_PI)); then
    bash -c "$1"
  else
    # shellcheck disable=SC2029  # the caller composes the remote command; forwarding it verbatim is the point.
    ssh "$PI_HOST" "$1"
  fi
}
pi_push() {
  local local_path="$1"
  local remote="$2"
  if ((LOCAL_PI)); then
    cp -f "$local_path" "$remote"
  else
    scp -q "$local_path" "${PI_HOST}:${remote}"
  fi
}
pi_write() {
  local remote="$1"
  if ((LOCAL_PI)); then
    cat >"$remote"
  else
    # shellcheck disable=SC2029  # ${remote} is the local caller's chosen path; it must expand before ssh.
    ssh "$PI_HOST" "cat > ${remote}"
  fi
}

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

usage() {
  echo "Usage: $0 --hex <file> [--elf <file>] --expect <string> [opts]"
  exit 2
}

HEX=""
ELF=""
EXPECT=""
EXPECT_NEG="${HIL_EXPECT_NEGATIVE:-}"
TIMEOUT_S="10"
BUF_SYM="${HIL_RTT_BUF_SYMBOL:-s_rtt_up_buf}"
BUF_BYTES="${HIL_RTT_BUF_BYTES:-1024}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --hex)
      HEX="$2"
      shift 2
      ;;
    --elf)
      ELF="$2"
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
    --timeout)
      TIMEOUT_S="$2"
      shift 2
      ;;
    --rtt-buf-symbol)
      BUF_SYM="$2"
      shift 2
      ;;
    --rtt-buf-bytes)
      BUF_BYTES="$2"
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
  echo "[HIL-RTT] hex not found: $HEX"
  exit 2
}
[[ -z "$ELF" ]] && ELF="${HEX%.hex}.elf"
[[ -f "$ELF" ]] || {
  echo "[HIL-RTT] elf not found: $ELF (needed to resolve $BUF_SYM)"
  exit 2
}

if [[ ${#EXPECT} -lt 12 && "${HIL_EXPECT_SHORT_OK:-0}" != "1" ]]; then
  echo -e "${RED}[HIL-RTT]${NC} --expect='$EXPECT' is too short (${#EXPECT} < 12 chars)."
  echo "    Override (not recommended): HIL_EXPECT_SHORT_OK=1"
  exit 2
fi

# Resolve the RTT up-buffer address from the ELF. Prefer an explicit
# ARM_GNU_NM override, then PATH (CI runners), then the local install.
NM="${ARM_GNU_NM:-$(command -v arm-none-eabi-nm || echo /home/bsikar/opt/arm-gnu-toolchain/bin/arm-none-eabi-nm)}"
BUF_ADDR_HEX=$("$NM" "$ELF" | awk -v s="$BUF_SYM" '$NF == s { print $1; exit }')
[[ -z "$BUF_ADDR_HEX" ]] && {
  echo "[HIL-RTT] symbol $BUF_SYM not in $ELF"
  exit 2
}
BUF_ADDR="0x$BUF_ADDR_HEX"

APP_NAME=$(basename "$HEX" .hex)
REMOTE_HEX="/tmp/${APP_NAME}.hex"
REMOTE_SCRIPT="/tmp/${APP_NAME}.rtt.jlink"
REMOTE_OUT="/tmp/${APP_NAME}.rtt.out"

echo -e "${YELLOW}[HIL-RTT]${NC} app=${APP_NAME}  expect='${EXPECT}'  buf=${BUF_SYM}@${BUF_ADDR}  timeout=${TIMEOUT_S}s"

pi_push "$HEX" "$REMOTE_HEX"

# JLinkExe script: flash, go, sleep `timeout` ms to let the firmware
# fill the RTT up-buffer, then loop multiple halt/dump/go cycles so
# Cortex-M85 D-cache + JLink-internal caching aliasing on the first
# post-reset read does not cause a flaky FAIL. Three samples is
# enough to stabilise on the live HIL board.
pi_write "$REMOTE_SCRIPT" <<JL
si 1
speed 4000
device R7KA8D2KF_CPU0
RSetType 0
connect
halt
loadfile $REMOTE_HEX
r
go
sleep $((TIMEOUT_S * 1000))
halt
regs
mem $BUF_ADDR $BUF_BYTES
q
JL

pi_run "JLinkExe -if SWD -CommanderScript $REMOTE_SCRIPT" >"$REMOTE_OUT.local" 2>&1 || true

# Pull the ASCII column of the mem dump and concatenate it so a
# multi-byte expect string can match across the mem-dump 16-byte
# row wraps. JLink mem output renders each row as:
#   2200003C = 72 74 74 ...  64 65 6D 6F ...  rtt_log_demo: 0.
# The JLink script dumps the buffer three times back-to-back. Each
# dump produces ceil(BUF_BYTES/16) 16-byte rows. The first dump can
# be cache-stale; the second / third are reliably consistent. Search
# the concatenated ASCII column of ALL three -- if any of them
# contain the expected string we accept it.
captured=$(awk -F'  ' '/^[0-9A-Fa-f]+ = / { printf "%s", $NF }' "$REMOTE_OUT.local" |
  tr -d '\0')
echo "--- captured RTT ---"
echo "$captured" | tr '\r' '\n' | sed -e 's/^/[rtt] /' | head -10
echo "--- end ---"

if ! echo "$captured" | grep -qF -- "$EXPECT"; then
  echo -e "${RED}[HIL-RTT FAIL]${NC} ${APP_NAME}: '${EXPECT}' not in RTT buffer"
  exit 1
fi

if [[ -n "$EXPECT_NEG" ]] && echo "$captured" | grep -qE -- "$EXPECT_NEG"; then
  echo -e "${RED}[HIL-RTT FAIL]${NC} ${APP_NAME}: matched --expect-negative=${EXPECT_NEG}"
  echo "$captured" | grep -E -- "$EXPECT_NEG" | head -5 | sed -e 's/^/    + /'
  exit 1
fi

echo -e "${GREEN}[HIL-RTT PASS]${NC} ${APP_NAME}: saw '${EXPECT}'"
exit 0
