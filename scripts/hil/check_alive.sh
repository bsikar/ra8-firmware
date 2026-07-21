#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# hil_check_alive.sh -- flash a firmware app, let it run for N seconds, then
# reconnect via J-Link and verify the CPU is still healthy (not in a hard
# fault, PC is in the MRAM region, watchdog hasn't reset us into a loop).
#
# This is the catch-all HIL verifier for apps that don't have a UART scrape
# or USB CDC echo to assert against -- LED blinkers, DAC waveform demos,
# CAN-loopback flag-toggling, ThreadX scheduling demos, etc. It's a weaker
# test than UART-string assertion but it's automatic and catches the broad
# regressions: HardFault, stack overflow, watchdog reset loop, NULL-pointer
# dereference, locked-up scheduler.
#
# What it checks (after the boot-seconds dwell):
#   1. J-Link can re-attach without "could not be halted" errors.
#   2. Program counter (PC) is in the MRAM code window
#      (0x02000000..0x020FFFFF), or the ITCM code window
#      (0x00000000..0x0000FFFF). HardFault handler addresses are rejected.
#   3. CycleCnt advances over the J-Link `go` window: proves forward
#      progress (not just "PC happens to land in MRAM at halt").
#   4. SCB->CFSR (Configurable Fault Status, 0xE000ED28) has no bits set
#      (UsageFault, BusFault, MemFault all latched here).
#   5. SCB->HFSR (Hard Fault Status, 0xE000ED2C) with DEBUGEVT (bit 31)
#      masked off has no bits set: bit 30 FORCED + bit 1 VECTTBL are the
#      meaningful HardFault indicators. DEBUGEVT is set whenever we halt
#      via SWD on debug-disabled silicon -- not a real fault.
#   6. UART capture during boot dwell contains no negative banner
#      ("FAIL", "panic", "failed", "NAK", "ERROR", "fault", "hardfault",
#      "stack overflow") -- catches silent-by-design apps that emit a
#      failure status banner but otherwise look alive to J-Link.
#
# Usage:
#   bash scripts/hil/check_alive.sh --hex /path/to/<app>.hex \
#                                   [--boot-seconds 2] \
#                                   [--app-name <name>]
#
# Environment overrides:
#   HIL_FAULT_EXPECTED=1  -- this app's success state is a latched
#                            configurable fault (mpu_partition_*). The
#                            CFSR check is skipped; instead a non-zero
#                            CFSR is REQUIRED, and the UART capture
#                            must contain "fault: handled" or similar
#                            (caller decides via uart_scrape mode).
#
# Exit:
#   0 = chip is alive (no fault, PC in expected region, no negative
#       banner)
#   1 = fault detected, PC in unexpected region, CycleCnt stuck, UART
#       emitted negative banner, or J-Link error
#   2 = usage error or Pi unreachable

set -euo pipefail

# Rig config (PI_HOST, JLINK_SN) comes from the gitignored .env, not the tree.
_hil_dir="$(dirname "${BASH_SOURCE[0]}")"
_hil_dir="$(cd "$_hil_dir" && pwd)"
# shellcheck source=scripts/hil/lib/rig_env.sh
source "$_hil_dir/lib/rig_env.sh"
rig_require PI_HOST JLINK_SN

# Detect when we are already running on the Pi itself (self-hosted CI
# runner, or running locally on the bench). In that case we must NOT
# `ssh $PI_HOST` -- mDNS resolution of the Pi's `.local` hostname can fail
# inside a self-hosted runner's network namespace, and we have direct
# access to the same files/devices anyway.
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
pi_run_stdin() {
  if ((LOCAL_PI)); then
    bash -s
  else
    ssh "$PI_HOST" "bash -s"
  fi
}
pi_pull() {
  local remote="$1"
  local local_path="$2"
  if ((LOCAL_PI)); then
    cp -f "$remote" "$local_path" 2>/dev/null || : >"$local_path"
  else
    scp -q "${PI_HOST}:${remote}" "$local_path" 2>/dev/null || : >"$local_path"
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

HEX=""
BOOT_S=2
APP_NAME=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --hex)
      HEX="$2"
      shift 2
      ;;
    --boot-seconds)
      BOOT_S="$2"
      shift 2
      ;;
    --app-name)
      APP_NAME="$2"
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
  echo "Usage: $0 --hex <file.hex> [--boot-seconds N]"
  exit 2
}
[[ -f "$HEX" ]] || {
  echo -e "${RED}[ERROR]${NC} hex not found: $HEX"
  exit 1
}
[[ -z "$APP_NAME" ]] && APP_NAME="$(basename "$HEX" .hex)"

# 1. Flash the app.
echo -e "${YELLOW}[alive]${NC} flashing ${APP_NAME}..."
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Suppress stdout (each successful flash prints a J-Link log path that
# we don't need in the CI scroll), but KEEP stderr so a real flash
# failure shows the underlying error instead of just "flash failed".
"${SCRIPT_DIR}/flash.sh" "$APP_NAME" >/dev/null || {
  echo -e "${RED}[alive]${NC} flash failed for ${APP_NAME}"
  exit 1
}

# 2. Capture UART during the boot dwell. We auto-detect the J-Link OB
# CDC port the same way hil_run.sh does -- ID_MODEL=J-Link in udevadm
# picks the right ttyACM when the chip's USBHS CDC has also enumerated.
# Run cat in the background on the Pi for BOOT_S seconds; halt before
# the J-Link probe begins, since J-Link halt freezes the UART side.
UART_CAP="/tmp/hil_alive_${APP_NAME}.uart"
UART_DEV=$(
  pi_run_stdin <<'REMOTE'
for dev in /dev/ttyACM*; do
    [[ -e "$dev" ]] || continue
    if udevadm info "$dev" 2>/dev/null | grep -q "ID_MODEL=J-Link"; then
        echo "$dev"
        exit 0
    fi
done
echo "/dev/ttyACM0"
REMOTE
)
UART_DEV="${UART_DEV:-/dev/ttyACM0}"
echo -e "${YELLOW}[alive]${NC} capturing UART on ${UART_DEV} for ${BOOT_S}s..."
pi_run "stty -F ${UART_DEV} 115200 raw -echo cs8 -cstopb -parenb 2>/dev/null; timeout ${BOOT_S} cat ${UART_DEV} > ${UART_CAP} 2>/dev/null || true"
pi_pull "${UART_CAP}" "${UART_CAP}"

# 3. Re-attach via J-Link, snapshot PC + fault status registers.
# 0x02000000 = MRAM base, 0x02100000 = MRAM end (RA8D2 has 1MB MRAM).
# 0xE000ED28 = SCB->CFSR (Configurable Fault Status).
# 0xE000ED2C = SCB->HFSR (Hard Fault Status).
# 1500ms `go` window: shorter values (200ms) sometimes captured a
# CycleCnt that hadn't advanced enough to distinguish "running" from
# "spinning on the same wfi" -- 1.5s is comfortably above the slowest
# realistic main-loop tick on this chip.
PI_TMP="/tmp/hil_alive_${APP_NAME}.jlink"
PI_LOG="/tmp/hil_alive_${APP_NAME}.log"
JLINK_SCRIPT=$(
  cat <<EOF
device ${JLINK_DEVICE}
si SWD
speed 1000
connect
halt
mem32 0xE000ED28 1
mem32 0xE000ED2C 1
regs
go
sleep 1500
halt
regs
q
EOF
)

if ((LOCAL_PI)); then
  echo "$JLINK_SCRIPT" >"$PI_TMP"
  JLinkExe -nogui 1 -SelectEmuBySN "${JLINK_SN}" -commanderscript "$PI_TMP" >"$PI_LOG" 2>&1 || true
else
  pi_write "${PI_TMP}" <<<"$JLINK_SCRIPT"
  pi_run "JLinkExe -nogui 1 -SelectEmuBySN ${JLINK_SN} -commanderscript ${PI_TMP} > ${PI_LOG} 2>&1 || true"
  pi_pull "${PI_LOG}" "/tmp/hil_alive_${APP_NAME}.log"
  PI_LOG="/tmp/hil_alive_${APP_NAME}.log"
fi

LOG_TEXT="$(cat "$PI_LOG")"

# Two PC + CycleCnt samples bracketing a 200ms `go` window. If the chip
# is making forward progress, CycleCnt increases.
#
# J-Link Commander's `regs` prints the register set twice in a row
# (FPU + integer banks each get a print pass), so a single regs call
# emits two identical "PC = ..., CycleCnt = ..." lines. We sample the
# FIRST distinct value (pre-go) and the LAST distinct value (post-go-
# sleep-halt); the values in between are just the duplicate prints
# from the same regs call.
mapfile -t PC_LIST < <(echo "$LOG_TEXT" | grep -oE 'PC[ ]*=[ ]*[0-9A-Fa-f]+' | grep -oE '[0-9A-Fa-f]+$' | tr 'a-f' 'A-F' | awk '!seen[$0]++')
mapfile -t CYC_LIST < <(echo "$LOG_TEXT" | grep -oE 'CycleCnt[ ]*=[ ]*[0-9A-Fa-f]+' | grep -oE '[0-9A-Fa-f]+$' | tr 'a-f' 'A-F' | awk '!seen[$0]++')
PC1="${PC_LIST[0]:-}"
PC2="${PC_LIST[$((${#PC_LIST[@]} > 1 ? ${#PC_LIST[@]} - 1 : 0))]:-${PC1}}"
CYC1="${CYC_LIST[0]:-}"
CYC2="${CYC_LIST[$((${#CYC_LIST[@]} > 1 ? ${#CYC_LIST[@]} - 1 : 0))]:-${CYC1}}"

# CFSR (active configurable-fault flags) -- bits cleared by firmware on
# normal boot; non-zero means an unhandled configurable fault.
CFSR_HEX="$(echo "$LOG_TEXT" | grep -iE '^E000ED28' | head -1 | awk '{print $3}' | tr 'a-f' 'A-F')"
# HFSR (hard fault flags) -- bit 31 DEBUGEVT is set on every SWD halt
# and is masked off here; bits we DO check are 30 FORCED (sub-fault
# escalated to HardFault) and 1 VECTTBL (vector-table read fault).
HFSR_HEX="$(echo "$LOG_TEXT" | grep -iE '^E000ED2C' | head -1 | awk '{print $3}' | tr 'a-f' 'A-F')"

ok=1
issues=()

if [[ -z "$PC1" || -z "$CYC1" ]]; then
  issues+=("could not parse PC/CycleCnt from J-Link output")
  ok=0
else
  pc1_dec=$((16#$PC1))
  pc2_dec=$((16#${PC2:-$PC1}))
  cyc1_dec=$((16#$CYC1))
  cyc2_dec=$((16#${CYC2:-$CYC1}))

  mram_lo=$((16#02000000))
  mram_hi=$((16#02100000))
  itcm_lo=$((16#00000000))
  itcm_hi=$((16#00010000))

  in_code_region() {
    local p="$1"
    ((p >= mram_lo && p < mram_hi)) && return 0
    ((p >= itcm_lo && p < itcm_hi)) && return 0
    return 1
  }

  if ! in_code_region "$pc1_dec"; then
    issues+=("PC1=0x${PC1} outside MRAM/ITCM")
    ok=0
  fi
  if ! in_code_region "$pc2_dec"; then
    issues+=("PC2=0x${PC2} outside MRAM/ITCM")
    ok=0
  fi
  # DWT CYCCNT is a free-running 32-bit counter: at 1 GHz it wraps
  # every ~4.3 s, so cyc2 < cyc1 is a normal wrap, not a stall. Use
  # the unsigned 32-bit delta -- only an exactly-zero delta means the
  # counter is genuinely frozen.
  cyc_delta=$(((cyc2_dec - cyc1_dec) & 0xFFFFFFFF))
  if ((cyc_delta == 0)); then
    issues+=("CycleCnt did not advance (0x${CYC1} -> 0x${CYC2}); CPU may be stuck")
    ok=0
  fi
fi

# Symbol-name check: PC inside MRAM is necessary but not sufficient.
# A chip looping in panic_halt() or Default_Handler() also has a PC in
# MRAM, but the chip is effectively dead. Resolve PC1 and PC2 to a
# symbol via the matching .elf and reject any address that lands in a
# known fault-spinner. The .elf sits next to the .hex (CMake emits both
# to .../build/<app>.{elf,hex}); skip the symbol check if .elf isn't
# alongside.
ELF="${HEX%.hex}.elf"
FAULT_SPINNER_RE='(panic_halt|halt_loop|exception_halt|Default_Handler|HardFault_Handler|MemManage_Handler|BusFault_Handler|UsageFault_Handler|SecureFault_Handler|spin_forever|hang_forever|_die|__assert_fail)'
if [[ -f "$ELF" ]] && command -v arm-none-eabi-addr2line >/dev/null 2>&1; then
  PC1_SYM="$(arm-none-eabi-addr2line -e "$ELF" -f "0x${PC1}" 2>/dev/null | head -1 || true)"
  PC2_SYM="$(arm-none-eabi-addr2line -e "$ELF" -f "0x${PC2}" 2>/dev/null | head -1 || true)"
  if echo "$PC1_SYM" | grep -qE "$FAULT_SPINNER_RE" ||
    echo "$PC2_SYM" | grep -qE "$FAULT_SPINNER_RE"; then
    issues+=("PC landed in fault spinner: PC1=${PC1_SYM:-?} PC2=${PC2_SYM:-?}")
    ok=0
  fi
fi

# CFSR is a strong real-fault indicator (not a debug artifact like
# HFSR.DEBUGEVT). If any configurable-fault bit is latched, treat as
# fail UNLESS the manifest opted into fault-expected (mpu_partition_*).
if [[ "${HIL_FAULT_EXPECTED:-0}" == "1" ]]; then
  # caller said a configurable fault IS the test result -- require
  # one rather than just ignoring CFSR; otherwise the app could
  # silently pass when its real failure mode is "no fault at all".
  if [[ -z "$CFSR_HEX" || "$((16#${CFSR_HEX:-0}))" == "0" ]]; then
    issues+=("HIL_FAULT_EXPECTED=1 but CFSR=0 (no fault latched)")
    ok=0
  fi
elif [[ -n "$CFSR_HEX" ]]; then
  cfsr_dec=$((16#$CFSR_HEX))
  if ((cfsr_dec != 0)); then
    issues+=("CFSR=0x${CFSR_HEX} non-zero (configurable fault latched)")
    ok=0
  fi
fi

# HFSR with DEBUGEVT (bit 31) masked off. DEBUGEVT fires on every SWD
# halt against debug-disabled silicon -- not a real fault. FORCED (bit
# 30) and VECTTBL (bit 1) are the meaningful HardFault indicators.
if [[ "${HIL_FAULT_EXPECTED:-0}" != "1" && -n "$HFSR_HEX" ]]; then
  hfsr_dec=$((16#$HFSR_HEX))
  hfsr_real=$((hfsr_dec & ~(1 << 31)))
  if ((hfsr_real != 0)); then
    issues+=("HFSR=0x${HFSR_HEX} non-zero with DEBUGEVT masked (real HardFault: FORCED/VECTTBL bits latched)")
    ok=0
  fi
fi

# UART capture negative-keyword scan. If the firmware emitted a
# failure banner during the boot dwell, alive-mode should NOT pass --
# that was the tz_secure_only_sd-class false-green: chip booted, J-Link
# saw a happy PC, but the UART had been screaming "sd: FAIL init" the
# whole time. Patterns are case-insensitive, word-bounded where it
# matters, and tuned to not match successful banners like
# "fault: handled" (used by mpu_partition_* under HIL_FAULT_EXPECTED).
UART_TEXT=""
if [[ -s "$UART_CAP" ]]; then
  UART_TEXT="$(cat "$UART_CAP" 2>/dev/null || true)"
fi
NEG_RE='\b(FAIL|FAILED|panic|NAK|ERROR|HardFault|hardfault|MemFault|BusFault|UsageFault|stack[ _-]?overflow)\b'
if [[ -n "$UART_TEXT" ]]; then
  NEG_HIT="$(echo "$UART_TEXT" | grep -iE "$NEG_RE" | head -3 || true)"
  if [[ -n "$NEG_HIT" ]]; then
    issues+=("UART emitted failure banner during boot dwell:")
    while IFS= read -r line; do
      issues+=("      ${line}")
    done <<<"$NEG_HIT"
    ok=0
  fi
fi

if ((ok == 1)); then
  echo -e "${GREEN}[alive]${NC} ${APP_NAME} PASS (PC=0x${PC1}->0x${PC2}, CycleCnt 0x${CYC1}->0x${CYC2})"
  exit 0
else
  echo -e "${RED}[alive]${NC} ${APP_NAME} FAIL"
  for i in "${issues[@]}"; do echo "    - $i"; done
  echo "    (J-Link log: ${PI_LOG})"
  [[ -s "$UART_CAP" ]] && echo "    (UART capture: ${UART_CAP})"
  exit 1
fi
