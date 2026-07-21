#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# hil_usb_test.sh -- HIL test runner for both USBFS and USBHS CDC echo on
# the EK-RA8D2.  Designed to run on the bench Pi (`$PI_HOST`) as the
# self-hosted gitea/github runner, or on a dev machine that can SSH there.
#
# What it does, for each controller:
#   1. Flash the corresponding "tz_secure_only_usb_fs*" app via the J-Link.
#   2. Power-cycle the board with `hil_tapo.sh board` so the new firmware boots.
#   3. Wait for the 1209:xxx CDC ACM device to appear on the right hub port.
#   4. Run scripts/hil/usb/benchmark.py against /dev/ttyACMx for correctness
#      + throughput.
#   5. PPPS-cycle the hub port to force USB re-enumeration without cutting
#      board power; assert the device reappears.
#
# Usage (run from the repo root):
#   bash scripts/hil/usb_test.sh                  # both FS and HS
#   bash scripts/hil/usb_test.sh --only fs
#   bash scripts/hil/usb_test.sh --only hs
#   bash scripts/hil/usb_test.sh --quick          # short correctness only
#
# Exit codes:
#   0 -- every selected test passed
#   1 -- at least one test failed
#   2 -- usage error or cannot reach the HIL Pi

set -euo pipefail

# Rig config (PI_HOST) comes from the gitignored .env, not the tree.
_hil_dir="$(dirname "${BASH_SOURCE[0]}")"
_hil_dir="$(cd "$_hil_dir" && pwd)"
# shellcheck source=scripts/hil/lib/rig_env.sh
source "$_hil_dir/lib/rig_env.sh"
rig_require PI_HOST
HUB_PORT_USBHS=1
HUB_PORT_USBFS=4
ENUM_WAIT_S=35
PPPS_REENUM_RETRIES=3

# Auto-detect if we are running on the Pi itself (so the script can serve
# as the body of a self-hosted CI job without SSH'ing to itself).
LOCAL_PI=0
if rig_is_local_pi; then
  LOCAL_PI=1
fi
# Wrappers: run something on the Pi (locally if we ARE the Pi, else via SSH).
# shellcheck disable=SC2029  # the caller composes the remote command; forwarding it verbatim is the point.
pi_run() { if ((LOCAL_PI)); then bash -c "$*"; else ssh "$PI_HOST" "$@"; fi; }
pi_cp() { if ((LOCAL_PI)); then cp "$1" "$2"; else scp -q "$1" "${PI_HOST}:${2}"; fi; }

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

ONLY=""
QUICK=""
ONLY_APP=""
APP_VIDPID=""
APP_HUB_PORT=""
APP_PPPS_MODE=""
APP_MPS_CHUNK=""
APP_STREAM_BYTES=""
APP_STREAM_FLOOR_KBS=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --only)
      ONLY="$2"
      shift 2
      ;;
    --quick)
      QUICK="--quick"
      shift
      ;;
    # Per-app overrides used by scripts/hil/all.sh (one app per run).
    --only-app)
      ONLY_APP="$2"
      shift 2
      ;;
    --vidpid)
      APP_VIDPID="$2"
      shift 2
      ;;
    --hub-port)
      APP_HUB_PORT="$2"
      shift 2
      ;;
    --ppps-mode)
      APP_PPPS_MODE="$2"
      shift 2
      ;;
    --mps-chunk)
      APP_MPS_CHUNK="$2"
      shift 2
      ;;
    --stream-bytes)
      APP_STREAM_BYTES="$2"
      shift 2
      ;;
    --stream-floor)
      APP_STREAM_FLOOR_KBS="$2"
      shift 2
      ;;
    -h | --help)
      sed -n '6,28p' "$0"
      exit 0
      ;;
    *)
      echo "Unknown arg: $1"
      exit 2
      ;;
  esac
done

if ((LOCAL_PI == 0)); then
  ssh -o ConnectTimeout=5 -o BatchMode=yes "$PI_HOST" true 2>/dev/null ||
    {
      echo -e "${RED}[ERROR]${NC} cannot reach ${PI_HOST}"
      exit 2
    }
fi

# Make sure the Pi has the latest copy of the benchmark scripts.
pi_cp scripts/hil/usb/benchmark.py /tmp/usb_benchmark.py
pi_cp scripts/hil/usb/stream_bench.py /tmp/usb_stream_bench.py

# Helper: chmod /dev/ttyACM* udev sometimes leaves at root-only because
# the device's deferred descriptor parse races the rule that adds the
# dialout group. The CI runs as the `star` user so we just relax the
# perms unconditionally before benchmarking.
relax_acm_perms() {
  pi_run "for d in /dev/ttyACM*; do sudo -n chmod 666 \$d 2>/dev/null || true; done"
}

# Resolve /dev/ttyACMx for the device with VID:PID == $vidpid.
find_ttyacm_for_vidpid() {
  local vidpid="$1"
  local vid="${vidpid%%:*}"
  pi_run "for d in /dev/ttyACM*; do udevadm info \$d 2>/dev/null | grep -q ID_VENDOR_ID=${vid} && { echo \$d; exit 0; }; done; exit 1"
}

# Wait until lsusb reports the given VID:PID on the right hub port.
wait_for_enum() {
  local vidpid="$1" timeout="${3:-$ENUM_WAIT_S}" t=0
  while ((t < timeout)); do
    if pi_run "lsusb 2>/dev/null | grep -q '${vidpid}'" 2>/dev/null; then
      return 0
    fi
    sleep 1
    ((t++))
  done
  return 1
}

# Steps 1-3: flash, then power-cycle until the device enumerates. Returns 1 if
# it never does -- there is nothing to test past that point.
_usb_flash_and_enumerate() {
  local name="$1" app="$2" vidpid="$3" hub_port="$4"

  echo -e "${YELLOW}[USB-${name}]${NC} step 1 -- flash"
  bash scripts/hil/flash.sh "${app}" >/dev/null

  echo -e "${YELLOW}[USB-${name}]${NC} step 2/3 -- power-cycle + wait for enumeration"
  for attempt in 1 2 3 4 5; do
    bash scripts/hil/tapo.sh board cycle >/dev/null
    if wait_for_enum "${vidpid}" "${hub_port}" "${ENUM_WAIT_S}"; then
      echo -e "${GREEN}[USB-${name}]${NC} enumerated (attempt ${attempt})"
      return 0
    fi
    # PPPS-bounce port between Tapo cycles for the next try; helps when
    # the host's xhci-hcd retried hard enough to confuse its internal
    # state machine.
    bash scripts/hil/ppps.sh cycle "${hub_port}" >/dev/null 2>&1 || true
  done
  echo -e "${RED}[USB-${name}]${NC} did NOT enumerate after 5 power-cycle attempts"
  return 1
}

# Step 4a: chapter-9 / data-path correctness over the CDC endpoint.
_usb_correctness() {
  local name="$1" dev="$2" mps_chunk="$3"

  echo -e "${YELLOW}[USB-${name}]${NC} step 4a -- correctness (usb_benchmark.py chunk=${mps_chunk}B)"
  if pi_run "python3 /tmp/usb_benchmark.py ${dev} ${QUICK} --throughput-chunk ${mps_chunk}"; then
    echo -e "${GREEN}[USB-${name}]${NC} correctness PASS"
    return 0
  fi
  echo -e "${RED}[USB-${name}]${NC} correctness FAIL"
  return 1
}

# Step 4b: sustained one-way throughput against a floor.
_usb_throughput() {
  local name="$1" dev="$2" stream_bytes="$3" stream_min_kbs="$4"
  local bench_out

  echo -e "${YELLOW}[USB-${name}]${NC} step 4b -- streaming throughput (${stream_bytes}B, floor ${stream_min_kbs} KB/s)"
  if ! bench_out=$(pi_run "python3 /tmp/usb_stream_bench.py ${dev} --bytes ${stream_bytes}" 2>&1); then
    echo "$bench_out"
    echo -e "${RED}[USB-${name}]${NC} streaming bench errored"
    return 1
  fi
  echo "$bench_out"
  # Parse the "one-way wire throughput : NNN.N KB/s" line and
  # compare against the floor. integer-ish compare to avoid bc.
  local kbs
  kbs=$(echo "$bench_out" | sed -nE 's/.*one-way wire throughput[^0-9]*([0-9]+).*/\1/p' | head -1)
  if [[ -n "$kbs" && "$kbs" -ge "$stream_min_kbs" ]]; then
    echo -e "${GREEN}[USB-${name}]${NC} throughput PASS (${kbs} KB/s >= ${stream_min_kbs} KB/s)"
    return 0
  fi
  echo -e "${RED}[USB-${name}]${NC} throughput FAIL (${kbs:-?} KB/s < ${stream_min_kbs} KB/s)"
  return 1
}

# Step 5: PPPS re-enumeration. USBHS uses the hardware uhubctl mechanism
# (PPPS), USBFS uses the host-side authorized sysfs toggle (--soft)
# because the device's USBFS PHY does not resync to a hub-side data
# toggle. Both achieve the same outcome: device forced to re-run
# chapter-9 enumeration without losing firmware state.
_usb_reenumerate() {
  local name="$1" vidpid="$2" hub_port="$3" ppps_mode="$4"

  # An array, not a string: an unquoted "" would pass hil_ppps.sh a spurious
  # empty argument, and quoting it would pass "" as a real argv entry.
  local soft_arg=()
  if [[ "$ppps_mode" == "soft" ]]; then
    soft_arg=(--soft)
  fi
  echo -e "${YELLOW}[USB-${name}]${NC} step 5 -- re-enumeration on port ${hub_port} (${ppps_mode})"
  for attempt in $(seq 1 "${PPPS_REENUM_RETRIES}"); do
    bash scripts/hil/ppps.sh ${soft_arg[@]+"${soft_arg[@]}"} cycle "${hub_port}" \
      >/dev/null 2>&1 || true
    if wait_for_enum "${vidpid}" "${hub_port}" "${ENUM_WAIT_S}"; then
      echo -e "${GREEN}[USB-${name}]${NC} re-enumeration OK (attempt ${attempt})"
      return 0
    fi
    echo -e "${YELLOW}[USB-${name}]${NC} re-enum attempt ${attempt} failed; retrying"
  done
  echo -e "${RED}[USB-${name}]${NC} re-enumeration FAILED after ${PPPS_REENUM_RETRIES} tries"
  return 1
}

run_one_controller() {
  local name="$1" app="$2" vidpid="$3" hub_port="$4" ppps_mode="${5:-hard}"
  local mps_chunk="${6:-64}"
  local stream_bytes="${7:-65536}" # streaming-bench payload size
  local stream_min_kbs="${8:-200}" # one-way throughput floor (KB/s)
  local rc=0

  echo
  echo -e "${CYAN}===========================================================${NC}"
  echo -e "${CYAN}[USB-${name}]${NC} app=${app}  vidpid=${vidpid}  hub_port=${hub_port}"
  echo -e "${CYAN}===========================================================${NC}"

  _usb_flash_and_enumerate "${name}" "${app}" "${vidpid}" "${hub_port}" || return 1

  relax_acm_perms
  local dev
  dev=$(find_ttyacm_for_vidpid "${vidpid}") || dev=""
  if [[ -z "$dev" ]]; then
    echo -e "${RED}[USB-${name}]${NC} no /dev/ttyACMx matches vid ${vidpid%%:*}"
    return 1
  fi
  echo -e "${YELLOW}[USB-${name}]${NC} device: ${dev}"

  _usb_correctness "${name}" "${dev}" "${mps_chunk}" || rc=1
  _usb_throughput "${name}" "${dev}" "${stream_bytes}" "${stream_min_kbs}" || rc=1
  _usb_reenumerate "${name}" "${vidpid}" "${hub_port}" "${ppps_mode}" || rc=1

  return "$rc"
}

overall=0

# Single-app mode (driven by scripts/hil/all.sh with a per-app manifest).
if [[ -n "$ONLY_APP" ]]; then
  # Required args
  : "${APP_VIDPID:?--vidpid required with --only-app}"
  : "${APP_HUB_PORT:?--hub-port required with --only-app}"
  APP_PPPS_MODE="${APP_PPPS_MODE:-hard}"
  APP_MPS_CHUNK="${APP_MPS_CHUNK:-512}"
  APP_STREAM_BYTES="${APP_STREAM_BYTES:-1048576}"
  APP_STREAM_FLOOR_KBS="${APP_STREAM_FLOOR_KBS:-2000}"
  # Pretty-print short controller name from VID:PID.
  label="$ONLY_APP"
  run_one_controller "$label" "$ONLY_APP" "$APP_VIDPID" "$APP_HUB_PORT" "$APP_PPPS_MODE" \
    "$APP_MPS_CHUNK" "$APP_STREAM_BYTES" "$APP_STREAM_FLOOR_KBS" || overall=1
else
  if [[ -z "$ONLY" || "$ONLY" == "fs" ]]; then
    # USBFS re-enumeration: hub-level PPPS (uhubctl) does not propagate
    # cleanly through the FS PHY, so use the host-side `authorized`
    # toggle ("soft"). MPS = 64. Streaming floor 250 KB/s (target ~360
    # measured, 1 MB/s theoretical wire).
    run_one_controller "FS" "tz_secure_only_usb_fs" "1209:000a" "$HUB_PORT_USBFS" "soft" \
      64 65536 250 || overall=1
  fi

  if [[ -z "$ONLY" || "$ONLY" == "hs" ]]; then
    # USBHS: hub-level PPPS (uhubctl) works fine. MPS = 512. Streaming
    # floor 2000 KB/s (target ~2660 measured, 30 MB/s theoretical wire).
    run_one_controller "HS" "tz_secure_only_usb_hs" "1209:000c" "$HUB_PORT_USBHS" "hard" \
      512 1048576 2000 || overall=1
  fi
fi

echo
if ((overall == 0)); then
  echo -e "${GREEN}[hil_usb_test DONE]${NC} all selected USB tests PASSED"
else
  echo -e "${RED}[hil_usb_test DONE]${NC} at least one USB test FAILED"
fi
exit "$overall"
