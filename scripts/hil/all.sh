#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# hil_all.sh -- run every HIL-able app under examples/ek_ra8d2/hw_validated/hil/.
#
# Auto-discovers: any directory under `hil/` containing both a CMakeLists.txt
# and a `hil.conf` is picked up. Apps without a hil.conf cause the run to
# fail (loud) so nothing slips through silently. Truly-human-only apps must
# live under `manual/` instead.
#
# Per-app `hil.conf` (sourced as bash) declares HOW the app is verified.
# Supported modes:
#
#   HIL_MODE=uart_scrape
#     HIL_EXPECT="some string"        -- substring that must appear on UART
#     HIL_EXPECT_NEGATIVE="re"        -- (optional) extended-regex of
#                                        failure banners that must NOT
#                                        appear; if matched, the run fails
#                                        even when HIL_EXPECT also matched.
#                                        Plug for the "expect overlaps a
#                                        failure banner" hole.
#     HIL_TIMEOUT_S=10                -- wait window after flashing
#
#   HIL_MODE=usb_cdc
#     HIL_VIDPID="1209:000c"       -- which CDC device to bind
#     HIL_HUB_PORT=1               -- VIA Labs hub port on 2-1.3 (1=HS, 4=FS)
#     HIL_PPPS_MODE=hard|soft      -- re-enum mechanism (hard=uhubctl, soft=authorized)
#     HIL_MPS_CHUNK=64|512         -- bulk MPS for the correctness chunk size
#     HIL_STREAM_BYTES=65536       -- streaming-bench payload
#     HIL_STREAM_FLOOR_KBS=250     -- one-way throughput floor (KB/s) to pass
#
#   HIL_MODE=usb_hid
#     HIL_VIDPID="1209:0001"       -- which HID device to expect (default
#                                     1209:0001). Flashes, then confirms
#                                     host-side that the kernel binds the
#                                     device as USB HID (hidraw + input
#                                     node). USB-class apps cannot use
#                                     jlink_memprobe -- halting the core
#                                     to read a counter stalls the SIE.
#
#   HIL_MODE=usb_msc
#     HIL_VIDPID="1209:000b"       -- which MSC device to expect (default
#                                     1209:000b). Flashes, then confirms
#                                     host-side that the kernel attached
#                                     the device as a SCSI block device
#                                     (the marker that the BOT INQUIRY +
#                                     READ_CAPACITY handshake completed,
#                                     i.e. the Issue #6 wedge cleared).
#
#   HIL_MODE=alive
#     HIL_BOOT_S=2                 -- seconds to let the chip run before
#                                     checking the CPU is still healthy
#
#   HIL_MODE=jlink_memprobe
#     HIL_PROBE_SYMBOL="g_tick"          -- name of a `volatile uint32_t`
#                                           the firmware increments in
#                                           its main loop / ISR
#     HIL_PROBE_MIN_ADVANCE=4            -- minimum delta over the window
#     HIL_PROBE_SECONDS=3                -- sample window length in seconds
#     HIL_PROBE_FAILURE_SYMBOL="g_err"   -- (optional) name of a failure
#                                           counter that must NOT advance
#                                           more than HIL_PROBE_MAX_FAILURE
#                                           (default 0). Pairs with the
#                                           primary counter for apps that
#                                           run a loopback / round-trip:
#                                           success counter must advance,
#                                           failure counter must stay 0.
#     HIL_PROBE_MAX_FAILURE=0            -- max allowed failure delta
#     For apps that should not pull in a UART (blink-class smoke tests)
#     or whose validation is a pure pass/fail counter (CAN loopback,
#     CRC verify, ...).
#
#   HIL_MODE=hil_eth_tcp
#     HIL_BOARD_IP="192.168.1.42"  -- IPv4 the firmware listens at
#     HIL_PORT=7                   -- listening TCP port
#     HIL_PROTO=tcp                -- "tcp" (echo), "udp" (echo), or "http"
#     HIL_PAYLOAD_BYTES=512        -- bytes to round-trip (tcp/udp only)
#     HIL_BOOT_TIMEOUT_S=25        -- wait for "eth: ready" banner
#     HIL_PROBE_TIMEOUT_S=10       -- wire-side probe deadline
#
# Usage:
#   bash scripts/hil/all.sh                  -- everything
#   bash scripts/hil/all.sh --only blink     -- one app
#   bash scripts/hil/all.sh --skip-build     -- assume binaries are built
#   bash scripts/hil/all.sh --mode uart_scrape  -- only one mode
#   bash scripts/hil/all.sh --list           -- enumerate apps + modes, no run
#
# Exit:
#   0 = every selected app passed
#   1 = at least one failed (or had no hil.conf)
#   2 = usage error
#
# Designed to be the one entry-point for both local dev and CI.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HIL_DIR="${REPO_ROOT}/examples/ek_ra8d2/hw_validated/hil"
# Rig config (PI_HOST, JLINK_SN) comes from the gitignored .env, not the tree.
# shellcheck source=scripts/hil/lib/rig_env.sh
source "${REPO_ROOT}/scripts/hil/lib/rig_env.sh"
rig_require JLINK_SN
PI_HOST="${PI_HOST:-}"

# Shared app discovery + hil.conf sourcing (also used by scripts/sim/sil_all.sh).
# shellcheck source=scripts/hil/lib/hil_conf.sh
source "${REPO_ROOT}/scripts/hil/lib/hil_conf.sh"

# When running ON the Pi itself, the lsusb/ttyACM probes and the dd target are
# already local. When running OFF the Pi (developer workstation), the helper
# scripts SSH out automatically -- but we still want to use the matching
# detection here to avoid double-hops.
LOCAL_PI=0
if rig_is_local_pi; then
  LOCAL_PI=1
fi
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

ONLY=""
MODE_FILTER=""
SKIP_BUILD=0
LIST_ONLY=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --only)
      ONLY="$2"
      shift 2
      ;;
    --mode)
      MODE_FILTER="$2"
      shift 2
      ;;
    --skip-build)
      SKIP_BUILD=1
      shift
      ;;
    --list)
      LIST_ONLY=1
      shift
      ;;
    -h | --help)
      sed -n '5,46p' "$0"
      exit 0
      ;;
    *)
      echo "Unknown arg: $1"
      exit 2
      ;;
  esac
done

# Auto-discover hil/<app>/ dirs (shared with sil_all.sh via hil_conf.sh).
declare -a APPS=()
while IFS= read -r name; do
  APPS+=("$name")
done < <(hil_discover_apps "$HIL_DIR")

if ((${#APPS[@]} == 0)); then
  echo -e "${RED}[hil_all]${NC} no apps found under ${HIL_DIR}"
  exit 1
fi

echo -e "${CYAN}[hil_all]${NC} discovered ${#APPS[@]} apps under hil/"

if ((LIST_ONLY)); then
  printf "%-40s %s\n" "APP" "MODE"
  for app in "${APPS[@]}"; do
    conf="${HIL_DIR}/${app}/hil.conf"
    if [[ -f "$conf" ]]; then
      mode="$(grep -E '^HIL_MODE=' "$conf" | head -1 | cut -d= -f2 | tr -d '"' || true)"
      printf "%-40s %s\n" "$app" "${mode:-(unset)}"
    else
      printf "%-40s %s\n" "$app" "(no hil.conf -- WILL FAIL)"
    fi
  done
  exit 0
fi

# Build everything first unless told otherwise. Build failures stop the run.
if ((SKIP_BUILD == 0)); then
  echo -e "${CYAN}[hil_all]${NC} building all apps under hil/"
  declare -a build_targets=()
  if [[ -n "$ONLY" ]]; then
    build_targets=("$ONLY")
  else
    build_targets=("${APPS[@]}")
  fi
  cd "$REPO_ROOT"
  # We pass everything to one `make` so the project's discovery sweeps once.
  if ! make -k "${build_targets[@]}" >/dev/null 2>&1; then
    echo -e "${RED}[hil_all]${NC} make failed for at least one app -- check 'make <app>' individually"
    exit 1
  fi
fi

# Per-app verifiers. Each takes the app name + sourced hil.conf vars and
# returns 0 on pass, non-zero on fail. Output a one-line PASS/FAIL summary.

run_uart_scrape() {
  local app="$1"
  local -a args=(
    --hex "${HIL_DIR}/${app}/build/${app}.hex"
    --expect "${HIL_EXPECT}"
    --timeout "${HIL_TIMEOUT_S:-10}"
  )
  if [[ -n "${HIL_EXPECT_NEGATIVE:-}" ]]; then
    args+=(--expect-negative "${HIL_EXPECT_NEGATIVE}")
  fi
  bash "${REPO_ROOT}/scripts/hil/run_direct.sh" "${args[@]}"
}

run_usb_cdc() {
  local app="$1"
  bash "${REPO_ROOT}/scripts/hil/usb_test.sh" \
    --only-app "${app}" \
    --vidpid "${HIL_VIDPID}" \
    --hub-port "${HIL_HUB_PORT}" \
    --ppps-mode "${HIL_PPPS_MODE:-hard}" \
    --mps-chunk "${HIL_MPS_CHUNK:-512}" \
    --stream-bytes "${HIL_STREAM_BYTES:-1048576}" \
    --stream-floor "${HIL_STREAM_FLOOR_KBS:-2000}"
}

run_usb_hid() {
  local app="$1"
  bash "${REPO_ROOT}/scripts/hil/hid_test.sh" \
    --app "${app}" \
    --vidpid "${HIL_VIDPID:-1209:0001}"
}

run_usb_msc() {
  local app="$1"
  bash "${REPO_ROOT}/scripts/hil/msc_test.sh" \
    --app "${app}" \
    --vidpid "${HIL_VIDPID:-1209:000b}"
}

run_alive() {
  local app="$1"
  local boot_s="${HIL_BOOT_S:-2}"
  bash "${REPO_ROOT}/scripts/hil/check_alive.sh" \
    --hex "${HIL_DIR}/${app}/build/${app}.hex" \
    --boot-seconds "${boot_s}"
}

run_jlink_memprobe() {
  local app="$1"
  local -a args=(
    --hex "${HIL_DIR}/${app}/build/${app}.hex"
    --symbol "${HIL_PROBE_SYMBOL}"
    --min-advance "${HIL_PROBE_MIN_ADVANCE:-4}"
    --seconds "${HIL_PROBE_SECONDS:-3}"
    --app-name "${app}"
  )
  if [[ -n "${HIL_PROBE_FAILURE_SYMBOL:-}" ]]; then
    args+=(--failure-symbol "${HIL_PROBE_FAILURE_SYMBOL}")
    args+=(--max-failure-advance "${HIL_PROBE_MAX_FAILURE:-0}")
  fi
  bash "${REPO_ROOT}/scripts/hil/jlink_memprobe.sh" "${args[@]}"
}

run_hil_eth_tcp() {
  local app="$1"
  bash "${REPO_ROOT}/scripts/hil/eth_tcp.sh" \
    --hex "${HIL_DIR}/${app}/build/${app}.hex" \
    --board-ip "${HIL_BOARD_IP}" \
    --port "${HIL_PORT}" \
    --proto "${HIL_PROTO:-tcp}" \
    --payload-bytes "${HIL_PAYLOAD_BYTES:-512}" \
    --boot-timeout "${HIL_BOOT_TIMEOUT_S:-25}" \
    --probe-timeout "${HIL_PROBE_TIMEOUT_S:-10}"
}

run_rtt_scrape() {
  local app="$1"
  bash "${REPO_ROOT}/scripts/hil/rtt_scrape.sh" \
    --hex "${HIL_DIR}/${app}/build/${app}.hex" \
    --elf "${HIL_DIR}/${app}/build/${app}.elf" \
    --expect "${HIL_EXPECT}" \
    --rtt-buf-symbol "${HIL_RTT_BUF_SYMBOL:-s_rtt_up_buf}" \
    --rtt-buf-bytes "${HIL_RTT_BUF_BYTES:-1024}" \
    --expect-negative "${HIL_EXPECT_NEGATIVE:-}" \
    --timeout "${HIL_TIMEOUT_S:-10}"
}

declare -i pass=0 fail=0 skipped=0
declare -a failed_apps=()

for app in "${APPS[@]}"; do
  if [[ -n "$ONLY" && "$ONLY" != "$app" ]]; then continue; fi

  conf="${HIL_DIR}/${app}/hil.conf"
  if [[ ! -f "$conf" ]]; then
    echo -e "${RED}[hil_all]${NC} ${app}: NO hil.conf (every app under hil/ must declare a HIL mode)"
    failed_apps+=("$app (missing hil.conf)")
    ((fail++)) || true
    continue
  fi

  # Source the manifest. Reset known vars first so values from a previous
  # app's conf cannot leak, then export them so per-mode runners invoked as
  # subprocesses (e.g. hil_check_alive.sh) can read them. Shared with
  # sil_all.sh via scripts/hil/lib/hil_conf.sh so both suites read the manifest
  # identically.
  hil_conf_load "$conf"

  if [[ -n "$MODE_FILTER" && "$MODE_FILTER" != "$HIL_MODE" ]]; then
    ((skipped++)) || true
    continue
  fi

  echo
  echo -e "${CYAN}[hil_all]${NC} =========================================="
  echo -e "${CYAN}[hil_all]${NC} ${app} (mode=${HIL_MODE})"
  echo -e "${CYAN}[hil_all]${NC} =========================================="

  # Issue #58: USB-mode tests flake when bus state from a prior test
  # (or even a non-USB test that left the device in an odd state) leaks
  # into this enumeration. Soft-PPPS the hub port before any usb_*
  # test so the kernel starts clean. Safe to repeat; the per-test
  # runners may PPPS again internally without harm.
  case "$HIL_MODE" in
    usb_cdc | usb_hid | usb_msc)
      bash "${REPO_ROOT}/scripts/hil/ppps.sh" --soft cycle "${HIL_HUB_PORT:-4}" \
        >/dev/null 2>&1 || true
      sleep 1
      ;;
  esac

  rc=0
  case "$HIL_MODE" in
    uart_scrape) run_uart_scrape "$app" || rc=$? ;;
    usb_cdc) run_usb_cdc "$app" || rc=$? ;;
    usb_hid) run_usb_hid "$app" || rc=$? ;;
    usb_msc) run_usb_msc "$app" || rc=$? ;;
    alive) run_alive "$app" || rc=$? ;;
    jlink_memprobe) run_jlink_memprobe "$app" || rc=$? ;;
    hil_eth_tcp) run_hil_eth_tcp "$app" || rc=$? ;;
    rtt_scrape) run_rtt_scrape "$app" || rc=$? ;;
    *)
      echo -e "${RED}[hil_all]${NC} ${app}: unknown HIL_MODE='${HIL_MODE}'"
      rc=99
      ;;
  esac

  if ((rc == 0)); then
    echo -e "${GREEN}[hil_all]${NC} ${app} PASS"
    ((pass++)) || true
  else
    echo -e "${RED}[hil_all]${NC} ${app} FAIL (rc=${rc})"
    failed_apps+=("$app (mode=${HIL_MODE})")
    ((fail++)) || true
  fi

  # ------------------------------------------------------------------------
  # Per-app post-test recovery hook. Apps that deliberately put the chip
  # into a state that gates the AHB-AP (e.g. LPM deep modes) set
  # HIL_POST_INITIALIZE=1 in their hil.conf. After running the test we
  # invoke `rfp-cli -erase-chip` (the boot-firmware Initialize command)
  # so the next app's flash has a haltable CPU. Without this, LPM-deep
  # demos cascade-fail every subsequent test until hil_flash.sh's
  # auto-recovery catches up. See scripts/hil/dlm_reset.sh for the
  # full DLM/AHB-AP recovery flow.
  if [[ "${HIL_POST_INITIALIZE:-0}" == "1" ]]; then
    echo -e "${CYAN}[hil_all]${NC} ${app}: HIL_POST_INITIALIZE=1 -- running rfp-cli -erase-chip..."
    # Detect Pi vs dev-machine (same heuristic as hil_flash.sh)
    if ((LOCAL_PI)); then
      rfp-cli -d ra -t "jlink:${JLINK_SN}" -if swd -s 1000000 -erase-chip \
        >"/tmp/hil_all_post_init_${app}.log" 2>&1 || true
    else
      ssh -o ConnectTimeout=5 -o BatchMode=yes "$PI_HOST" \
        "rfp-cli -d ra -t jlink:${JLINK_SN} -if swd -s 1000000 -erase-chip" \
        >"/tmp/hil_all_post_init_${app}.log" 2>&1 || true
    fi
    if grep -q "Operation successful" "/tmp/hil_all_post_init_${app}.log" 2>/dev/null; then
      echo -e "${GREEN}[hil_all]${NC} ${app}: post-test Initialize OK"
    else
      echo -e "${YELLOW}[hil_all]${NC} ${app}: post-test Initialize did not report success"
      echo -e "${YELLOW}[hil_all]${NC} (see /tmp/hil_all_post_init_${app}.log)"
    fi
  fi
done

echo
echo "==================================================="
echo "  hil_all: ${pass} passed, ${fail} failed, ${skipped} skipped"
echo "==================================================="
if ((fail > 0)); then
  echo "  FAILED apps:"
  for a in "${failed_apps[@]}"; do echo "    - $a"; done
  exit 1
fi
exit 0
