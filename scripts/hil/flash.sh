#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# hil_flash.sh -- Build (optional) and flash a firmware app to the EK-RA8D2
# via the Pi HIL host over SSH.  No UART verification is performed; the board
# is simply programmed and released from reset.
#
# Usage (run from the repo root on the dev machine):
#   /bin/bash -p scripts/hil/flash.sh <app>
#   /bin/bash -p scripts/hil/flash.sh lcd_demo
#
# The hex is expected at:
#   examples/<tier>/.../<app>/build/<app>.hex
#
# If the hex is not found the script offers to build first.
#
# Exit codes:
#   0  -- flash succeeded
#   1  -- flash failed or hex not found
#   2  -- usage error or Pi unreachable

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

  # Rig config (PI_HOST, JLINK_SN) comes from the gitignored .env, not the tree.
  _hil_dir="$(dirname "${BASH_SOURCE[0]}")"
  _hil_dir="$(cd "$_hil_dir" && pwd)"
  # shellcheck source=scripts/hil/lib/rig_env.sh
  source "$_hil_dir/lib/rig_env.sh"
  rig_require PI_HOST JLINK_SN

  GREEN='\033[0;32m'
  RED='\033[0;31m'
  YELLOW='\033[1;33m'
  NC='\033[0m'

  [[ $# -eq 1 ]] || {
    echo "Usage: $0 <app>"
    exit 2
  }
  APP_ID="$1"

  SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

  APP_DIR="$(python3 "$ROOT/scripts/dev/ra8_apps.py" dir "$APP_ID" 2>/dev/null || true)"
  APP="$(python3 "$ROOT/scripts/dev/ra8_apps.py" name "$APP_ID" 2>/dev/null || true)"

  if [[ -z "$APP_DIR" || -z "$APP" ]]; then
    echo -e "${RED}[ERROR]${NC} app '${APP_ID}' not found under examples/"
    exit 1
  fi

  HEX="${APP_DIR}/build/${APP}.hex"

  if [[ ! -f "$HEX" ]]; then
    echo -e "${YELLOW}[hil_flash]${NC} hex not found: $HEX"
    echo -e "${YELLOW}[hil_flash]${NC} building ${APP_ID}..."
    /bin/bash -p "$ROOT/scripts/dev/run_just.sh" --justfile "$ROOT/justfile" apps::build "$APP_ID"
  fi

  [[ -f "$HEX" ]] || {
    echo -e "${RED}[ERROR]${NC} build failed -- $HEX still missing"
    exit 1
  }

  echo -e "${YELLOW}[hil_flash]${NC} app=${APP_ID}"

  # ---- 0a. Bench mutual exclusion ----------------------------------------------
  # One actor at a time on the physical bench: the J-Link, its VCOM console, the
  # C6 on Pmod1 and the board plug are one assembly and none of them is
  # separable. The hold lives exactly as long as this script does. See
  # scripts/hil/bench.sh.
  # shellcheck source=scripts/hil/lib/bench_lock.sh
  source "$_hil_dir/lib/bench_lock.sh"
  ra8_bench_require "flash ${APP_ID}" || exit $?

  # ---- 0b. Anti-recovery pre-flash guard ---------------------------------------
  # Inspect the FULL image (pre-strip) and the source tree BEFORE programming, so
  # a lockdown value in the disable-initialize / DLM-lock / permanent-block-protect
  # option-setting region can never reach the board. See preflash_guard.sh.
  # shellcheck source=scripts/hil/lib/preflash_guard.sh
  source "$_hil_dir/lib/preflash_guard.sh"
  ra8_preflash_guard "$HEX" || exit $?

  # ---- 1. Strip OFS sections ---------------------------------------------------
  # OFS sections at 0x0300A100+ cause J-Link RAMCode to timeout during Prepare()
  # when TrustZone option bytes are involved.  Strip them so J-Link only programs
  # the MRAM bank at 0x02000000.
  #
  # SWD speed: RA8D2 boots from the internal ~4 MHz oscillator after SYSRESETREQ.
  # J-Link's RAMCode runs at this low clock and requires speed <= 1000 kHz to
  # communicate reliably.  Using 4000 kHz causes RAMCode timeout.
  ELF="${APP_DIR}/build/${APP}.elf"
  STRIPPED_HEX="$(mktemp "/tmp/hil_${APP}_mram.XXXXXX.hex")"
  trap 'rm -f "$STRIPPED_HEX"' EXIT
  OFS_ARGS=('--remove-section=.option_setting*')
  if [[ -f "$ELF" ]]; then
    arm-none-eabi-objcopy "${OFS_ARGS[@]}" -O ihex "$ELF" "$STRIPPED_HEX" 2>/dev/null ||
      cp "$HEX" "$STRIPPED_HEX"
  else
    arm-none-eabi-objcopy -I ihex "${OFS_ARGS[@]}" -O ihex "$HEX" "$STRIPPED_HEX" 2>/dev/null ||
      cp "$HEX" "$STRIPPED_HEX"
  fi

  # Keep direct bench-side developer/operator invocation working. CI runs on the
  # dev-box listener and therefore takes the remote path below.
  RUN_LOCAL=0
  if rig_is_local_pi; then
    RUN_LOCAL=1
  fi

  # Per-invocation staging paths. These used to be fixed (/tmp/hil_<app>_mram.hex
  # and friends), so two actors flashing the same app raced on the same file and
  # each could read the other's bytes -- and a stale one from a crashed run was
  # indistinguishable from a fresh one. The bench lock makes that collision rare;
  # a unique path per invocation makes it impossible.
  REMOTE_HEX="/tmp/hil_${APP}_mram.$$.hex"
  LOG="/tmp/hil_jlink_${APP}.$$.log"
  INIT_LOG="/tmp/hil_flash_init_${APP}.$$.log"

  if ((RUN_LOCAL == 0)); then
    # ---- 2. Check Pi reachable -----------------------------------------------
    ssh -o ConnectTimeout=5 -o BatchMode=yes "$PI_HOST" true 2>/dev/null ||
      {
        echo -e "${RED}[ERROR]${NC} cannot reach ${PI_HOST}"
        exit 2
      }

    # ---- 3. Copy hex to Pi ---------------------------------------------------
    echo -e "${YELLOW}[hil_flash]${NC} uploading hex..."
    scp -q "$STRIPPED_HEX" "${PI_HOST}:${REMOTE_HEX}"
  else
    # Local on the Pi: both paths are already on this filesystem. They differ
    # now that STRIPPED_HEX is a mktemp, but the guard stays -- `cp` refuses a
    # self-copy outright, and a future edit that makes them equal again should
    # not turn into a hard failure mid-flash.
    if [[ "$STRIPPED_HEX" != "$REMOTE_HEX" ]]; then
      cp "$STRIPPED_HEX" "$REMOTE_HEX"
    fi
  fi

  # ---- 4. Flash via J-Link (local on Pi, or via SSH) ---------------------------
  echo -e "${YELLOW}[hil_flash]${NC} flashing..."
  flash_cmds() {
    cat <<EOF
device ${JLINK_DEVICE}
si SWD
speed 1000
connect
halt
loadfile ${REMOTE_HEX}
r
g
q
EOF
  }
  post_check() {
    local log="$1"
    VTREF=$(grep -oP 'VTref=\K[0-9.]+V' "$log" | head -1 || echo "unknown")
    echo "    VTref : ${VTREF}"
    echo "    log   : ${log}"
    # Success = "O.K." present (the Downloading-file completion marker)
    # AND no terminal-fatal error patterns. We do NOT fail on "^Error"
    # alone because JLink prints recoverable error lines (e.g.
    # "Error: Failed to initialize DAP" followed by "Attach to CPU
    # failed. Trying connect under reset.") that resolve successfully
    # on retry, with "O.K." appearing later in the same log.
    if grep -qiE "Could not connect to the target device|RAMCode did not respond|Could not load|Failed to read memory|Could not find core in Coresight" "$log"; then
      echo "---- J-Link log (fatal errors detected) ----" >&2
      grep -iE "^Error|Warning|could not|failed|O\.K\.|VTref|Cortex|DAP|AP\[|loadfile|Downloading" \
        "$log" >&2 || cat "$log" >&2
      echo "---------------------------------------" >&2
      return 1
    fi
    if ! grep -q "O\.K\." "$log"; then
      echo "---- J-Link log (no O.K. confirm -- flash never completed) ----" >&2
      cat "$log" >&2
      echo "---------------------------------------" >&2
      return 1
    fi
    return 0
  }

  # Auto-recovery via rfp-cli -erase-chip (boot-firmware Initialize) when
  # the chip is in a TrustZone-locked / LPM-stuck state that gates the
  # AHB-AP. See scripts/hil/dlm_reset.sh for the full DLM recovery flow.
  attempt_recover() {
    local log="$1"
    if grep -qiE "could not be halted|Failed to configure AP|Failed to power up DAP|Failed to initialize DAP|Could not read CPUID register|Attach to CPU failed|Could not find core in Coresight" "$log"; then
      echo "[hil_flash] J-Link halt/DAP failure detected -- running rfp-cli -erase-chip (Initialize) auto-recovery..." >&2
      rfp-cli -d ra -t "jlink:${JLINK_SN}" -if swd -s 1000000 -erase-chip \
        >"${INIT_LOG}" 2>&1 || true
      if grep -q "Operation successful" "${INIT_LOG}"; then
        echo "[hil_flash] Initialize succeeded -- retrying flash..." >&2
        return 0
      else
        echo "[hil_flash] Initialize failed -- see ${INIT_LOG}" >&2
        return 1
      fi
    fi
    return 1
  }

  if ((RUN_LOCAL)); then
    TMP=$(mktemp)
    trap 'rm -f "$TMP"' EXIT
    flash_cmds >"$TMP"
    JLinkExe -nogui 1 -SelectEmuBySN "${JLINK_SN}" -commanderscript "$TMP" >"$LOG" 2>&1 || true
    if ! post_check "$LOG"; then
      if attempt_recover "$LOG"; then
        JLinkExe -nogui 1 -SelectEmuBySN "${JLINK_SN}" -commanderscript "$TMP" >"$LOG" 2>&1 || true
        post_check "$LOG" || exit 1
      else
        exit 1
      fi
    fi
  else
    # shellcheck disable=SC2087  # local vars (LOG, JLINK_SN, APP) expand client-side; remote vars are escaped
    ssh "$PI_HOST" /bin/bash -p <<REMOTE
set -uo pipefail
TMP=\$(mktemp)
LOG="${LOG}"
trap 'rm -f "\$TMP"' EXIT
cat > "\$TMP" <<JLINK
$(flash_cmds)
JLINK

# ---- attempt 1: regular flash --------------------------------------------------
JLinkExe -nogui 1 -SelectEmuBySN ${JLINK_SN} -commanderscript "\$TMP" > "\$LOG" 2>&1 || true
VTREF=\$(grep -oP 'VTref=\K[0-9.]+V' "\$LOG" | head -1 || echo "unknown")
echo "    VTref : \${VTREF}"
echo "    log   : \${LOG}"

# Detect the "chip stuck in LPM / TrustZone-locked" failure mode -- the
# Cortex-M85 AHB-AP is gated so JLink halt fails. The recovery is the
# Renesas Flash Programmer Initialize command (rfp-cli -erase-chip),
# which transitions OEM_PL0/PL1 back to OEM_PL2 and clears MRAM, making
# the next halt succeed. See scripts/hil/dlm_reset.sh for the full
# DLM recovery flow.
if grep -qiE "could not be halted|Failed to configure AP|Failed to power up DAP|Failed to initialize DAP|Could not read CPUID register|Attach to CPU failed|Could not find core in Coresight" "\$LOG"; then
    echo "[hil_flash] J-Link halt/DAP failure detected -- running rfp-cli -erase-chip (Initialize) auto-recovery..." >&2
    rfp-cli -d ra -t jlink:${JLINK_SN} -if swd -s 1000000 -erase-chip > "${INIT_LOG}" 2>&1 || true
    if grep -q "Operation successful" "${INIT_LOG}"; then
        echo "[hil_flash] Initialize succeeded -- retrying flash..." >&2
        JLinkExe -nogui 1 -SelectEmuBySN ${JLINK_SN} -commanderscript "\$TMP" > "\$LOG" 2>&1 || true
    else
        echo "[hil_flash] Initialize failed -- see ${INIT_LOG}" >&2
    fi
fi

# Final result check. Success = "O.K." present (the Downloading-file
# completion marker) AND no terminal-fatal error patterns.
#
# We do NOT fail on "^Error" alone because JLink prints recoverable
# error lines (e.g. "Error: Failed to initialize DAP" followed by
# "Attach to CPU failed. Trying connect under reset.") that resolve
# successfully on retry, with "O.K." appearing later. The terminal-
# fatal patterns below only fire when the entire connect attempt
# never recovered.
if grep -qiE "Could not connect to the target device|RAMCode did not respond|Could not load|Failed to read memory|Could not find core in Coresight" "\$LOG"; then
    echo "---- J-Link log (fatal errors detected) ----" >&2
    grep -iE "^Error|Warning|could not|failed|O\.K\.|VTref|Cortex|DAP|AP\[|loadfile|Downloading" \
        "\$LOG" >&2 || cat "\$LOG" >&2
    echo "---------------------------------------" >&2
    echo "(full log at \${LOG} on Pi)" >&2
    exit 1
fi
if ! grep -q "O\.K\." "\$LOG"; then
    echo "---- J-Link log (no O.K. confirm -- flash never completed) ----" >&2
    cat "\$LOG" >&2
    echo "---------------------------------------" >&2
    exit 1
fi
REMOTE
  fi

  echo -e "${GREEN}[hil_flash DONE]${NC} ${APP_ID} is running on the board"
else
  [[ "$-" == *p* ]]
fi
