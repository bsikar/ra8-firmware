#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# hil_all.sh -- run every HIL-able app under examples/ek_ra8d2/hw_validated/hil/.
#
# Auto-discovers: any directory under `hil/` containing both a CMakeLists.txt
# and a `hil.conf` is picked up. Apps without a hil.conf cause the run to
# fail (loud) so nothing slips through silently. Truly-human-only apps must
# live under `manual/` instead.
#
# `--dir <path-under-repo-root>` points the SAME runner at a different tier.
# There is one HIL runner, not one per bench configuration: `just hil::c6`
# is this script aimed at examples/ek_ra8d2/hw_validated/c6/, which needs a
# DIP-switch setting (SW4-4 OFF) that takes Arduino and mikroBUS off the board
# for every other app, so it cannot share a pass with the default tier. A
# second copy of this file would be a second place for the discovery, the
# bench hold and the six verifiers to drift.
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
#   HIL_MODE=c6_camera_livestream
#     HIL_SELF_BUILD=1              -- build in a temporary credential-free
#                                      tree owned by the dedicated verifier
#     HIL_FRAME_WIDTH=320            -- decoded still-frame width
#     HIL_FRAME_HEIGHT=240           -- decoded still-frame height
#     Runs the combined OV5640 + ESP32-C6 HTTP/JPEG end-to-end verifier.
#
# Usage:
#   /bin/bash -p scripts/hil/all.sh                  -- everything
#   /bin/bash -p scripts/hil/all.sh --only blink     -- one app
#   /bin/bash -p scripts/hil/all.sh --only blink,pdm_mic_demo
#                                            -- an ordered subset
#   /bin/bash -p scripts/hil/all.sh --dir examples/ek_ra8d2/hw_validated/c6
#                                            -- a different tier (see just hil::c6)
#   /bin/bash -p scripts/hil/all.sh --skip-build     -- assume binaries are built
#   /bin/bash -p scripts/hil/all.sh --mode uart_scrape  -- only one mode
#   /bin/bash -p scripts/hil/all.sh --list           -- enumerate apps + modes, no run
#
# Exit:
#   0 = every selected app passed
#   1 = at least one failed (or had no hil.conf)
#   2 = usage error
#
# Designed to be the one entry-point for both local dev and CI.

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

  REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd -P)"
  HIL_DIR="${REPO_ROOT}/examples/ek_ra8d2/hw_validated/hil"
  LOW_POWER_RECOVERY_APP="blink"
  # Rig config (PI_HOST, JLINK_SN) comes from the gitignored .env, not the tree.
  # shellcheck source=scripts/hil/lib/rig_env.sh
  source "${REPO_ROOT}/scripts/hil/lib/rig_env.sh"
  rig_require JLINK_SN
  PI_HOST="${PI_HOST:-}"

  # Shared app discovery + hil.conf sourcing (also used by scripts/emu/eil_all.sh).
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
  CYAN='\033[0;36m'
  NC='\033[0m'

  ONLY=""
  MODE_FILTER=""
  SKIP_BUILD=0
  LIST_ONLY=0
  DIR_OVERRIDE=""
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --dir)
        DIR_OVERRIDE="$2"
        shift 2
        ;;
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
        sed -n '5,55p' "$0"
        exit 0
        ;;
      *)
        echo "Unknown arg: $1"
        exit 2
        ;;
    esac
  done

  # A tier override is resolved (and existence-checked) here rather than at the
  # default assignment above, so the default stays the plain
  # HIL_DIR="${REPO_ROOT}/..." line that check_hil_eil_parity.py parses to learn
  # this harness's root. A gate that cannot find that line stops policing the
  # EIL==HIL invariant, which is the failure mode this tree keeps re-learning.
  if [[ -n "$DIR_OVERRIDE" ]]; then
    HIL_DIR="$(cd "${REPO_ROOT}/${DIR_OVERRIDE}" 2>/dev/null && pwd -P)" || {
      echo -e "${RED}[hil_all]${NC} --dir ${DIR_OVERRIDE} does not exist under the repo root"
      exit 2
    }
    case "$HIL_DIR" in
      "${REPO_ROOT}/examples/ek_ra8d2/"*) ;;
      *)
        echo -e "${RED}[hil_all]${NC} --dir must resolve under examples/ek_ra8d2"
        exit 2
        ;;
    esac
  fi

  # Auto-discover hil/<app>/ dirs (shared with eil_all.sh via hil_conf.sh).
  declare -a APPS=()
  while IFS= read -r name; do
    APPS+=("$name")
  done < <(hil_discover_apps "$HIL_DIR")

  if ((${#APPS[@]} == 0)); then
    echo -e "${RED}[hil_all]${NC} no apps found under ${HIL_DIR}"
    exit 1
  fi
  app_is_selected() {
    local app="$1"
    [[ -z "$ONLY" || ",${ONLY}," == *",${app},"* ]]
  }

  if [[ -n "$ONLY" ]]; then
    IFS=',' read -r -a requested_apps <<<"$ONLY"
    for requested in "${requested_apps[@]}"; do
      [[ -n "$requested" ]] || {
        echo -e "${RED}[hil_all]${NC} --only contains an empty app name"
        exit 2
      }
      only_found=0
      for app in "${APPS[@]}"; do
        [[ "$app" == "$requested" ]] && only_found=1
      done
      if ((only_found == 0)); then
        echo -e "${RED}[hil_all]${NC} --only app '$requested' was not discovered"
        exit 2
      fi
    done
  fi

  echo -e "${CYAN}[hil_all]${NC} discovered ${#APPS[@]} apps under ${HIL_DIR#"${REPO_ROOT}/"}"

  # ---- bench mutual exclusion --------------------------------------------------
  # ONE hold for the whole suite, taken here and inherited by every per-mode
  # runner through RA8_BENCH_LOCK_ID, so every discovered app is one indivisible
  # occupancy of the bench rather than one acquisition race per app.
  # 2h: the sum of every HIL_TIMEOUT_S is already 43 minutes of verify budget
  # before a single flash, and hil.yml's 90-minute cap is a tight bound, not a
  # generous one.
  #
  # Listing is not a bench operation, so `--list` deliberately runs before this.
  # shellcheck source=scripts/hil/lib/bench_lock.sh
  source "${REPO_ROOT}/scripts/hil/lib/bench_lock.sh"

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

  # Every real suite run uses GCC for an optional build and binutils while
  # validating or pre-stripping images. Bind all of them to the same 13.3
  # release before any app-specific runner inherits PATH. `--list` stays a
  # discovery-only operation and deliberately exits before this requirement.
  # shellcheck source=scripts/ci/lib/arm_toolchain.sh
  source "${REPO_ROOT}/scripts/ci/lib/arm_toolchain.sh"
  use_pinned_arm_toolchain
  require_pinned_arm_toolchain

  # Build everything locally before taking the bench. The batch builder owns the
  # bounded pool and shared archive; compilation must never consume rig time.
  if ((SKIP_BUILD == 0)); then
    echo -e "${CYAN}[hil_all]${NC} building all apps under hil/"
    declare -a build_targets=()
    app_root="${HIL_DIR#"${REPO_ROOT}/examples/"}"
    need_low_power_recovery=0
    for app in "${APPS[@]}"; do
      if ! app_is_selected "$app"; then
        continue
      fi
      conf="${HIL_DIR}/${app}/hil.conf"
      if [[ -f "$conf" ]]; then
        hil_conf_load "$conf"
        if [[ "${HIL_POST_POWER_CYCLE_HALT:-0}" == "1" ]]; then
          need_low_power_recovery=1
        fi
        if [[ "${HIL_SELF_BUILD:-0}" == "1" ]]; then
          echo -e "${CYAN}[hil_all]${NC} ${app}: verifier owns its private build"
          continue
        fi
      fi
      build_targets+=("${app_root}/${app}")
    done
    if ((need_low_power_recovery)); then
      recovery_dir="${HIL_DIR}/${LOW_POWER_RECOVERY_APP}"
      [[ -d "$recovery_dir" ]] || {
        echo -e "${RED}[hil_all]${NC} missing low-power recovery app: ${recovery_dir}"
        exit 2
      }
      recovery_target="${app_root}/${LOW_POWER_RECOVERY_APP}"
      recovery_already_selected=0
      for target in "${build_targets[@]}"; do
        [[ "$target" == "$recovery_target" ]] && recovery_already_selected=1
      done
      if ((recovery_already_selected == 0)); then
        build_targets+=("$recovery_target")
      fi
    fi
    if ((${#build_targets[@]} > 0)); then
      printf '%s\0' "${build_targets[@]}" |
        RA8_SELECTED_APP_FLOOR="${#build_targets[@]}" BUILD_TYPE=RelWithDebInfo \
          /bin/bash -p "$REPO_ROOT/scripts/builders/all_examples.sh" --selected0 || exit $?
    else
      echo -e "${CYAN}[hil_all]${NC} every selected verifier owns its private build"
    fi
  fi

  ra8_bench_require "HIL suite: ${#APPS[@]} apps${ONLY:+ (only $ONLY)}" 2h || exit $?

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
    if [[ "${HIL_PROVISION_WIFI:-0}" == "1" ]]; then
      args+=(--provision-wifi)
    fi
    /bin/bash -p "${REPO_ROOT}/scripts/hil/run_direct.sh" "${args[@]}"
  }

  run_usb_cdc() {
    local app="$1"
    /bin/bash -p "${REPO_ROOT}/scripts/hil/usb_test.sh" \
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
    /bin/bash -p "${REPO_ROOT}/scripts/hil/hid_test.sh" \
      --app "${app}" \
      --vidpid "${HIL_VIDPID:-1209:0001}"
  }

  run_usb_msc() {
    local app="$1"
    /bin/bash -p "${REPO_ROOT}/scripts/hil/msc_test.sh" \
      --app "${app}" \
      --vidpid "${HIL_VIDPID:-1209:000b}"
  }

  run_alive() {
    local app="$1"
    local boot_s="${HIL_BOOT_S:-2}"
    /bin/bash -p "${REPO_ROOT}/scripts/hil/check_alive.sh" \
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
    /bin/bash -p "${REPO_ROOT}/scripts/hil/jlink_memprobe.sh" "${args[@]}"
  }

  run_hil_eth_tcp() {
    local app="$1"
    /bin/bash -p "${REPO_ROOT}/scripts/hil/eth_tcp.sh" \
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
    /bin/bash -p "${REPO_ROOT}/scripts/hil/rtt_scrape.sh" \
      --hex "${HIL_DIR}/${app}/build/${app}.hex" \
      --elf "${HIL_DIR}/${app}/build/${app}.elf" \
      --expect "${HIL_EXPECT}" \
      --rtt-buf-symbol "${HIL_RTT_BUF_SYMBOL:-s_rtt_up_buf}" \
      --rtt-buf-bytes "${HIL_RTT_BUF_BYTES:-1024}" \
      --expect-negative "${HIL_EXPECT_NEGATIVE:-}" \
      --timeout "${HIL_TIMEOUT_S:-10}"
  }

  run_c6_camera_livestream() {
    local app="$1"
    /bin/bash -p "${REPO_ROOT}/scripts/hil/camera_livestream.sh" \
      --app-dir "${HIL_DIR}/${app}" \
      --width "${HIL_FRAME_WIDTH:-320}" \
      --height "${HIL_FRAME_HEIGHT:-240}"
  }

  declare -i pass=0 fail=0 skipped=0
  declare -a failed_apps=()

  for app in "${APPS[@]}"; do
    if ! app_is_selected "$app"; then continue; fi

    conf="${HIL_DIR}/${app}/hil.conf"
    if [[ ! -f "$conf" ]]; then
      echo -e "${RED}[hil_all]${NC} ${app}: NO hil.conf (every app under hil/ must declare a HIL mode)"
      failed_apps+=("$app (missing hil.conf)")
      fail=$((fail + 1))
      continue
    fi

    # Source the manifest. Reset known vars first so values from a previous
    # app's conf cannot leak, then export them so per-mode runners invoked as
    # subprocesses (e.g. hil_check_alive.sh) can read them. Shared with
    # eil_all.sh via scripts/hil/lib/hil_conf.sh so both suites read the manifest
    # identically.
    hil_conf_load "$conf"

    if [[ -n "$MODE_FILTER" && "$MODE_FILTER" != "$HIL_MODE" ]]; then
      skipped=$((skipped + 1))
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
        /bin/bash -p "${REPO_ROOT}/scripts/hil/ppps.sh" --soft cycle "${HIL_HUB_PORT:-4}" \
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
      c6_camera_livestream) run_c6_camera_livestream "$app" || rc=$? ;;
      *)
        echo -e "${RED}[hil_all]${NC} ${app}: unknown HIL_MODE='${HIL_MODE}'"
        rc=99
        ;;
    esac

    if ((rc == 0)); then
      echo -e "${GREEN}[hil_all]${NC} ${app} PASS"
      pass=$((pass + 1))
    else
      echo -e "${RED}[hil_all]${NC} ${app} FAIL (rc=${rc})"
      failed_apps+=("$app (mode=${HIL_MODE})")
      fail=$((fail + 1))
    fi

    # ------------------------------------------------------------------------
    # Per-app post-test recovery hook. Firmware that deliberately gates the
    # AHB-AP cannot be fixed by calling rfp-cli after the fact: the programmer
    # itself gets E100000E because the debug path is asleep. The recovery
    # boundary is a real POR followed by a fast J-Link halt and programming the
    # next selected image in that same connection. If it cannot be established,
    # stop instead of cascading a known-bad board state into later apps.
    if [[ "${HIL_POST_POWER_CYCLE_HALT:-0}" == "1" ]]; then
      recovery_app="$LOW_POWER_RECOVERY_APP"
      recovery_hex="${HIL_DIR}/${recovery_app}/build/${recovery_app}.hex"
      echo -e "${CYAN}[hil_all]${NC} ${app}: restoring debug and installing ${recovery_app}..."
      if [[ -f "$recovery_hex" ]] &&
        /bin/bash -p "${REPO_ROOT}/scripts/hil/exit_low_power.sh" "$app" "$recovery_hex"; then
        echo -e "${GREEN}[hil_all]${NC} ${app}: post-test recovery installed ${recovery_app}"
      else
        echo -e "${RED}[hil_all]${NC} ${app}: post-test low-power recovery FAILED"
        if ((rc == 0)); then
          pass=$((pass - 1))
          fail=$((fail + 1))
          failed_apps+=("$app (post-test low-power recovery)")
        fi
        echo -e "${RED}[hil_all]${NC} stopping: the next app cannot safely use an unverified target"
        break
      fi
    fi

    # DLM/TrustZone test images have a different recovery contract from low
    # power: their debug path is intentionally protected, but the boot-firmware
    # Initialize command remains available. Preserve that destructive hook as a
    # separate manifest knob and require positive success before continuing.
    if [[ "${HIL_POST_INITIALIZE:-0}" == "1" ]]; then
      init_log="/tmp/hil_all_post_init_${app}.$$.log"
      echo -e "${CYAN}[hil_all]${NC} ${app}: running declared rfp-cli Initialize..."
      init_rc=0
      if ((LOCAL_PI)); then
        rfp-cli -d ra -t "jlink:${JLINK_SN}" -if swd -s 1000000 -erase-chip \
          >"$init_log" 2>&1 || init_rc=$?
      else
        ssh -o ConnectTimeout=5 -o BatchMode=yes "$PI_HOST" \
          "rfp-cli -d ra -t jlink:${JLINK_SN} -if swd -s 1000000 -erase-chip" \
          >"$init_log" 2>&1 || init_rc=$?
      fi
      if ((init_rc == 0)) && grep -q "Operation successful" "$init_log"; then
        echo -e "${GREEN}[hil_all]${NC} ${app}: post-test Initialize OK"
      else
        echo -e "${RED}[hil_all]${NC} ${app}: post-test Initialize FAILED (rc=${init_rc}, log=${init_log})"
        if ((rc == 0)); then
          pass=$((pass - 1))
          fail=$((fail + 1))
          failed_apps+=("$app (post-test Initialize)")
        fi
        echo -e "${RED}[hil_all]${NC} stopping: the next app cannot safely use an unverified target"
        break
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
else
  [[ "$-" == *p* ]]
fi
