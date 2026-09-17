#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# hil_rtt_scrape.sh -- flash a firmware .hex, let it run, then read its
# SEGGER RTT up-buffer directly via J-Link `mem` reads (avoids
# JLinkRTTLogger's reset-on-connect quirk). Mirrors hil_run_direct.sh's
# CLI so the batch HIL runner can dispatch to it the same way.
#
# Usage:
#   scripts/hil/rtt_scrape.sh --hex <path/to/app.hex>      \
#                             --elf <path/to/app.elf>      \
#                             --expect <string>            \
#                             [--rtt-buf-symbol s_rtt_up_buf] \
#                             [--rtt-buf-bytes 1024]       \
#                             [--expect-negative <regex>]  \
#                             [--timeout 10]

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

  set -euo pipefail

  # Rig config (PI_HOST) comes from the gitignored .env, not the tree.
  _hil_dir="$(dirname "${BASH_SOURCE[0]}")"
  _hil_dir="$(cd "$_hil_dir" && pwd)"
  # Resolve nm from the pinned 13.3 release rather than falling back to an
  # obsolete developer-specific path. The full probe also prevents mixed GCC
  # and binutils from making ELF inspection depend on the launching shell.
  # shellcheck source=scripts/ci/lib/arm_toolchain.sh
  source "$_hil_dir/../ci/lib/arm_toolchain.sh"
  use_pinned_arm_toolchain
  require_pinned_arm_toolchain
  # shellcheck source=scripts/hil/lib/rig_env.sh
  source "$_hil_dir/lib/rig_env.sh"
  rig_require PI_HOST JLINK_DEVICE

  # ---- bench mutual exclusion --------------------------------------------------
  # One actor at a time on the physical bench. The hold lives exactly as long as
  # this script does -- it is a live process on a kernel flock, not a lease -- so
  # nothing here can leave the bench stale. See scripts/hil/bench.sh.
  # shellcheck source=scripts/hil/lib/bench_lock.sh
  source "$_hil_dir/lib/bench_lock.sh"
  ra8_bench_require "hil rtt scrape $*" || exit $?

  # Keep direct bench-side developer/operator invocation working. In that case
  # skip the `ssh $PI_HOST` round-trip because the process already has access to
  # the same files, J-Link, and TTY. CI itself runs on the dev-box listener and
  # takes the remote branch below.
  LOCAL_PI=0
  if rig_is_local_pi; then
    LOCAL_PI=1
  fi
  pi_run() {
    if ((LOCAL_PI)); then
      /bin/bash -p -c "$1"
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

  # Inspect the full image before staging it or creating the J-Link load script.
  # shellcheck source=scripts/hil/lib/preflash_guard.sh
  source "$_hil_dir/lib/preflash_guard.sh"
  ra8_preflash_guard "$HEX" || exit $?

  if [[ ${#EXPECT} -lt 12 && "${HIL_EXPECT_SHORT_OK:-0}" != "1" ]]; then
    echo -e "${RED}[HIL-RTT]${NC} --expect='$EXPECT' is too short (${#EXPECT} < 12 chars)."
    echo "    Override (not recommended): HIL_EXPECT_SHORT_OK=1"
    exit 2
  fi

  # Resolve the RTT up-buffer address with nm from the verified toolchain PATH.
  BUF_ADDR_HEX=$(arm-none-eabi-nm "$ELF" | awk -v s="$BUF_SYM" '$NF == s { print $1; exit }')
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
device ${JLINK_DEVICE}
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
else
  [[ "$-" == *p* ]]
fi
