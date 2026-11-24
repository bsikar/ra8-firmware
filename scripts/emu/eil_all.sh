#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# eil_all.sh -- EIL (emulator-in-the-loop): run every HIL app in the
# tools/ra8_emulator HEADLESS and verify the SAME per-app hil.conf
# expectations the real-hardware suite (scripts/hil/all.sh) checks -- with NO
# board attached. It is the hardware-free mirror of hil_all.sh, fast enough to
# gate every push in CI.
#
# For each app under examples/ek_ra8d2/hw_validated/hil/ -- plus any RA8P1/NPU
# foundation app under examples/ra8p1_foundation/ that carries a hil.conf, which
# ra8_emulator emulates via `--device ra8p1` (declared per app as HIL_EMU_ARGS) --
# eil_all.sh sources the same hil.conf (via scripts/hil/lib/hil_conf.sh, shared with
# hil_all.sh) and dispatches by HIL_MODE:
#
#   uart_scrape / alive   Build the .elf, boot it in ra8_emulator headless with
#                         --console and --until, and PASS iff HIL_EXPECT appears
#                         AND (if set) HIL_EXPECT_NEGATIVE does NOT, AND the run
#                         did not fault and reached a clean stop/budget.
#
#   jlink_memprobe        Run with --dump-sym <sym> and --stop-sym <sym> <N> until
#                         HIL_PROBE_SYMBOL reaches HIL_PROBE_MIN_ADVANCE, then
#                         PASS iff it did AND HIL_PROBE_FAILURE_SYMBOL stayed
#                         <= HIL_PROBE_MAX_FAILURE AND the run did not fault.
#
#   rtt_scrape            Run with --ms 3500 so RTT lines flush into the end-of-run
#                         `SEGGER RTT:` report (no UART prompt needed).
#
#   hil_eth_tcp           Run headless with --console and --until HIL_EXPECT,
#                         asserting both HIL_EXPECT and peer byte-exact echo MATCH.
#
# A SKIP is a clearly-reported "ra8_emulator cannot check this mode" -- never a
# false pass. Only apps in an EIL-capable mode (uart_scrape / alive /
# jlink_memprobe / rtt_scrape / hil_eth_tcp) can FAIL the suite; SKIPs do not.
#
# EIL==HIL discipline (enforced by scripts/checks/check_hil_eil_parity.py):
# "EIL==HIL" means ra8_emulator must reproduce the ACTUAL silicon behaviour of
# every HIL path -- bugs INCLUDED.
#
# Usage:
#   /bin/bash -p scripts/emu/eil_all.sh                  # every app
#   /bin/bash -p scripts/emu/eil_all.sh -j 8             # cap at 8
#   /bin/bash -p scripts/emu/eil_all.sh --only blink     # one app
#   /bin/bash -p scripts/emu/eil_all.sh --mode uart_scrape
#   /bin/bash -p scripts/emu/eil_all.sh --serial         # deterministic
#   /bin/bash -p scripts/emu/eil_all.sh --skip-build     # reuse ELFs
#   /bin/bash -p scripts/emu/eil_all.sh --list           # list apps
#
# Budgets (env overridable):
#   EIL_JOBS               parallel workers (default: ra8_max_jobs)
#   EIL_UART_MS            uart_scrape/alive/eth virtual-time cap, ms (default 5000)
#   EIL_UART_INSTRUCTIONS  fallback instruction cap when no --until/ms cap applies
#   EIL_PROBE_MS           jlink_memprobe minimum virtual-time cap, ms (default 8000 via --stop-sym)
#
# Exit: 0 = every EIL-capable app passed; 1 = at least one failed; 2 = usage.

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

  REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
  HIL_DIR="${REPO_ROOT}/examples/ek_ra8d2/hw_validated/hil"
  EIL_RA8P1_DIR="${REPO_ROOT}/examples/ra8p1_foundation"
  EMU_DIR="${REPO_ROOT}/tools/ra8_emulator"

  # ra8_max_jobs -- the ONE canonical bounded-parallelism width.
  # shellcheck source=scripts/lib/parallelism.sh
  source "${REPO_ROOT}/scripts/lib/parallelism.sh"

  # Shared app discovery + hil.conf sourcing (also used by scripts/hil/all.sh).
  # shellcheck source=scripts/hil/lib/hil_conf.sh
  source "${REPO_ROOT}/scripts/hil/lib/hil_conf.sh"

  export LC_ALL=C

  GREEN='\033[0;32m'
  RED='\033[0;31m'
  YELLOW='\033[1;33m'
  CYAN='\033[0;36m'
  NC='\033[0m'

  # Deterministic virtual-time and instruction budgets. --until / --stop-sym
  # exit immediately when the expected banner or counter target is met.
  EIL_UART_MS="${EIL_UART_MS:-5000}"
  EIL_UART_INSTRUCTIONS="${EIL_UART_INSTRUCTIONS:-40000000}"
  EIL_PROBE_MS="${EIL_PROBE_MS:-8000}"
  EIL_RTT_MS="${EIL_RTT_MS:-3500}"

  # Per-app device fixtures shared with matrix.sh.
  # shellcheck source=scripts/emu/emu_fixtures.sh
  . "${REPO_ROOT}/scripts/emu/emu_fixtures.sh"

  # Per-app instruction override for heavy software-crypto or flash RMW apps.
  eil_app_max_instructions() { # <app> -> instruction budget on stdout
    local override="${HIL_EMU_MAX_CHUNKS:-}"
    if [ -n "$override" ]; then
      # Legacy hil.conf files expressed raised budgets in chunk units; scale
      # conservatively so RMW-heavy apps always clear their passes.
      if [ "$override" -lt 1000000 ] 2>/dev/null; then
        printf '%s' "$((override * 200))"
      else
        printf '%s' "$((override * 8))"
      fi
      return
    fi
    case "$1" in
      psa_crypto_hil) printf '80000000' ;;
      fs_format_mount) printf '100000000' ;;
      *) printf '%s' "$EIL_UART_INSTRUCTIONS" ;;
    esac
  }

  # Bake a FAT card carrying FONT.OTF once (for sd_font_render).
  build_font_card() {
    local font="${REPO_ROOT}/apps/shared_libs/third_party/litehtml/containers/test/fonts/ahem.ttf"
    local mk="${REPO_ROOT}/tools/mkfontimg"
    [ -f "$font" ] || return 0
    (cd "${mk}" && zig build --prefix build) >/dev/null 2>&1 || return 0
    EIL_FONT_IMG="${EIL_RUN_DIR}/font.img"
    "${mk}/build/bin/mkfontimg" "$font" "$EIL_FONT_IMG" FONT.OTF >/dev/null 2>&1 || EIL_FONT_IMG=""
  }

  # Ensure ereader_shelf's baked MRAM library header (library.h) exists.
  build_shelf_library() {
    local hdr="${HIL_DIR}/ereader_shelf/inc/library.h"
    [ -f "$hdr" ] && return 0
    echo -e "${CYAN}[eil_all]${NC} generating ereader_shelf library.h (build_books.sh --shelf-only) ..."
    if ! /bin/bash -p "${REPO_ROOT}/scripts/builders/books.sh" --shelf-only \
      >/tmp/eil_books.log 2>&1; then
      echo -e "${YELLOW}[eil_all]${NC} could not generate ereader_shelf library.h (see" \
        "/tmp/eil_books.log); ereader_shelf will report a build failure" >&2
    fi
  }

  eil_app_dir() { # <app> -> app directory on stdout
    if [ -d "${HIL_DIR}/$1" ]; then
      printf '%s' "${HIL_DIR}/$1"
    else
      printf '%s' "${EIL_RA8P1_DIR}/$1"
    fi
  }

  eil_ns_elf_args() { # <secure-elf> -> "--ns <ns-elf>" on stdout (or empty)
    local ns="${1%.elf}_ns.elf"
    if [ -f "$ns" ]; then
      printf -- '--ns %s' "$ns"
    fi
  }

  eil_cpu1_elf_args() { # <primary-elf> -> "--cpu1 <cpu1-elf>" on stdout (or empty)
    local cpu1="${1%.elf}_cpu1.elf"
    if [ -f "$cpu1" ]; then
      printf -- '--cpu1 %s' "$cpu1"
    fi
  }

  # Normalize HIL_EMU_ARGS flags to the current ra8_emulator CLI spelling.
  eil_map_emu_args() { # <raw-args> -> normalized args on stdout
    local tok
    for tok in $1; do
      case "$tok" in
        --usbhs-loop) printf -- '--usb-loop ' ;;
        *) printf -- '%s ' "$tok" ;;
      esac
    done
  }

  eil_collect_extra_args() { # <app> <elf> -> populates caller `extra` array
    local raw
    raw="$(emu_extra_args "$1") $(eil_ns_elf_args "$2") $(eil_cpu1_elf_args "$2") $(eil_map_emu_args "${HIL_EMU_ARGS:-}")"
    extra=()
    read -r -a extra <<<"$raw"
  }

  # =============================================================================
  # Worker verdict helpers
  # =============================================================================

  eil_fault_line() { # <captured-output> -> nonempty fault description on stdout
    local out="$1"
    if grep -q 'zig core: BKPT halted' <<<"$out"; then
      printf 'fault: BKPT halt (assert / give-up)'
    elif grep -qE 'zig core: (unknown encoding|EPSR\.T clear|bus fault|invalid exception return|unaligned MemA access|stack limit overrun|no vector table)' <<<"$out"; then
      printf 'fault: cpu/memory stop'
    fi
  }

  verdict_uart() { # <app> <elf> <result_file>
    local app="$1" elf="$2" rf="$3"
    local expect="${HIL_EXPECT:-}" neg="${HIL_EXPECT_NEGATIVE:-}"
    local budget out fault
    local -a extra=() run_flags=()
    if [ -z "$expect" ]; then
      eil_emit "$rf" FAIL "$app" "hil.conf has no HIL_EXPECT"
      return
    fi
    eil_collect_extra_args "$app" "$elf"
    if [ "${HIL_MODE:-}" = "rtt_scrape" ]; then
      run_flags=(--ms "$EIL_RTT_MS" --console)
    else
      budget="$(eil_app_max_instructions "$app")"
      run_flags=(--until "$expect" --console)
      if [ -n "${HIL_EMU_MAX_CHUNKS:-}" ] || [ "$budget" -gt "$EIL_UART_INSTRUCTIONS" ]; then
        run_flags+=(--instructions "$budget")
      else
        run_flags+=(--ms "$EIL_UART_MS")
      fi
    fi
    out="$("$EIL_EMU" "$elf" "${run_flags[@]}" ${extra[@]+"${extra[@]}"} </dev/null 2>&1 || true)"
    fault="$(eil_fault_line "$out")"
    if [ -n "$fault" ]; then
      eil_emit "$rf" FAIL "$app" "$fault"
      return
    fi
    if [ -n "$neg" ] && grep -qE "$neg" <<<"$out"; then
      eil_emit "$rf" FAIL "$app" "matched HIL_EXPECT_NEGATIVE"
      return
    fi
    if ! grep -qF "$expect" <<<"$out"; then
      eil_emit "$rf" FAIL "$app" "output lacks '${expect}' (ra8_emulator gap?)"
      return
    fi
    if ! grep -qE 'zig core: ran [0-9]+ instructions clean|stopped clean ' <<<"$out"; then
      eil_emit "$rf" FAIL "$app" "banner seen but run did not end cleanly"
      return
    fi
    eil_emit "$rf" PASS "$app" "uart '${expect}'"
  }

  verdict_eth_tcp() { # <app> <elf> <result_file>
    local app="$1" elf="$2" rf="$3"
    local expect="${HIL_EXPECT:-}" neg="${HIL_EXPECT_NEGATIVE:-}"
    local peer_ok="echo MATCH"
    local out fault
    local -a extra=()
    if [ -z "$expect" ]; then
      eil_emit "$rf" FAIL "$app" "hil.conf has no HIL_EXPECT"
      return
    fi
    eil_collect_extra_args "$app" "$elf"
    out="$("$EIL_EMU" "$elf" --until "$expect" --ms "$EIL_UART_MS" --console \
      ${extra[@]+"${extra[@]}"} </dev/null 2>&1 || true)"
    fault="$(eil_fault_line "$out")"
    if [ -n "$fault" ]; then
      eil_emit "$rf" FAIL "$app" "$fault"
      return
    fi
    if [ -n "$neg" ] && grep -qE "$neg" <<<"$out"; then
      eil_emit "$rf" FAIL "$app" "matched HIL_EXPECT_NEGATIVE"
      return
    fi
    if ! grep -qF "$expect" <<<"$out"; then
      eil_emit "$rf" FAIL "$app" "UART lacks '${expect}' (echo never served?)"
      return
    fi
    if ! grep -qF "$peer_ok" <<<"$out"; then
      eil_emit "$rf" FAIL "$app" "peer NET TCP report lacks '${peer_ok}' (echo bytes differ?)"
      return
    fi
    if ! grep -qE 'zig core: ran [0-9]+ instructions clean|stopped clean ' <<<"$out"; then
      eil_emit "$rf" FAIL "$app" "banner seen but run did not end cleanly"
      return
    fi
    eil_emit "$rf" PASS "$app" "uart '${expect}' + peer byte-exact echo MATCH"
  }

  verdict_alive() { # <app> <elf> <result_file>
    local app="$1" elf="$2" rf="$3"
    local expect="${HIL_EXPECT:-}" neg="${HIL_EXPECT_NEGATIVE:-}"
    local out fault
    local -a extra=() run_flags=(--ms "$EIL_UART_MS" --console)
    eil_collect_extra_args "$app" "$elf"
    if [ -n "$expect" ]; then
      run_flags+=(--until "$expect")
    fi
    out="$("$EIL_EMU" "$elf" "${run_flags[@]}" ${extra[@]+"${extra[@]}"} </dev/null 2>&1 || true)"
    fault="$(eil_fault_line "$out")"
    if [ -n "$fault" ]; then
      eil_emit "$rf" FAIL "$app" "$fault (not alive)"
      return
    fi
    if [ -n "$neg" ] && grep -qE "$neg" <<<"$out"; then
      eil_emit "$rf" FAIL "$app" "matched HIL_EXPECT_NEGATIVE"
      return
    fi
    if [ -n "$expect" ] && ! grep -qF "$expect" <<<"$out"; then
      eil_emit "$rf" FAIL "$app" "alive but UART lacks '${expect}'"
      return
    fi
    # shellcheck disable=SC2016  # the quotes are inside a ${var:+...} alternate,
    # where they are literal output and ${expect} still expands. Verified: a set
    # $expect renders "uart 'foo'", an empty one renders nothing.
    eil_emit "$rf" PASS "$app" "alive (no give-up fault)${expect:+, uart '${expect}'}"
  }

  verdict_memprobe() { # <app> <elf> <result_file>
    local app="$1" elf="$2" rf="$3"
    local sym="${HIL_PROBE_SYMBOL:-}" min="${HIL_PROBE_MIN_ADVANCE:-4}"
    local fsym="${HIL_PROBE_FAILURE_SYMBOL:-}" fmax="${HIL_PROBE_MAX_FAILURE:-0}"
    local -a args=() extra=()
    local out fault val fval probe_ms
    if [ -z "$sym" ]; then
      eil_emit "$rf" FAIL "$app" "hil.conf has no HIL_PROBE_SYMBOL"
      return
    fi
    args=(--dump-sym "$sym" --stop-sym "$sym" "$min" --console)
    if [ -n "$fsym" ]; then
      args+=(--dump-sym "$fsym")
    fi
    if [ -n "${HIL_EMU_MAX_CHUNKS:-}" ]; then
      args+=(--instructions "$(eil_app_max_instructions "$app")")
    else
      probe_ms="$(((${HIL_PROBE_BOOT_S:-0} + ${HIL_PROBE_SECONDS:-5} + 3) * 1000))"
      [ "$probe_ms" -lt "$EIL_PROBE_MS" ] && probe_ms="$EIL_PROBE_MS"
      args+=(--ms "$probe_ms")
    fi
    eil_collect_extra_args "$app" "$elf"
    out="$("$EIL_EMU" "$elf" "${args[@]}" ${extra[@]+"${extra[@]}"} </dev/null 2>&1 || true)"
    fault="$(eil_fault_line "$out")"
    if [ -n "$fault" ]; then
      eil_emit "$rf" FAIL "$app" "$fault"
      return
    fi
    val="$(eil_dump_sym_value "$out" "$sym")"
    if [ -z "$val" ]; then
      eil_emit "$rf" FAIL "$app" "counter '${sym}' unresolved in ra8_emulator"
      return
    fi
    if [ "$val" -lt "$min" ]; then
      eil_emit "$rf" FAIL "$app" "${sym}=${val} < min ${min} (ra8_emulator gap?)"
      return
    fi
    if [ -n "$fsym" ]; then
      fval="$(eil_dump_sym_value "$out" "$fsym")"
      fval="${fval:-0}"
      if [ "$fval" -gt "$fmax" ]; then
        eil_emit "$rf" FAIL "$app" "failure counter ${fsym}=${fval} > ${fmax}"
        return
      fi
      eil_emit "$rf" PASS "$app" "${sym}=${val}>=${min}, ${fsym}=${fval}<=${fmax}"
      return
    fi
    eil_emit "$rf" PASS "$app" "${sym}=${val}>=${min}"
  }

  # Extract the decimal value ra8_emulator's --dump-sym printed for a symbol:
  #   "  dump-sym      : g_foo @0x2200 = 42 (0x0000002A)"  -> "42"
  eil_dump_sym_value() { # <captured-output> <symbol> -> decimal value on stdout
    sed -n "s/.*dump-sym[[:space:]]*: $2 @0x[0-9A-Fa-f]* = \([0-9][0-9]*\) .*/\1/p" <<<"$1" | head -1
  }

  eil_emit() { # <result_file> <status> <app> <detail>
    printf '%s|%s|%s|%s\n' "$2" "$3" "${HIL_MODE:-unknown}" "$4" >"$1"
  }

  run_one() {
    local app="$1"
    local appdir
    appdir="$(eil_app_dir "$app")"
    local conf="${appdir}/hil.conf"
    local rf="${EIL_RUN_DIR}/${app}.result"
    local elf

    if [ ! -f "$conf" ]; then
      printf 'FAIL|%s|missing|no hil.conf (every hil/ app must declare a HIL mode)\n' "$app" >"$rf"
      eil_progress "$app" FAIL
      return 0
    fi
    hil_conf_load "$conf"

    case "${HIL_MODE:-}" in
      uart_scrape | alive | jlink_memprobe | rtt_scrape | hil_eth_tcp) : ;;
      *)
        eil_emit "$rf" FAIL "$app" "unknown HIL_MODE='${HIL_MODE:-}'"
        eil_progress "$app" FAIL
        return 0
        ;;
    esac

    if [ "${EIL_SKIP_BUILD:-0}" != "1" ]; then
      if ! /bin/bash -p "$REPO_ROOT/scripts/dev/run_just.sh" apps::build "$app" \
        >"${EIL_RUN_DIR}/${app}.build.log" 2>&1; then
        eil_emit "$rf" FAIL "$app" "BUILD FAIL (see ${app}.build.log)"
        eil_progress "$app" FAIL
        return 0
      fi
    fi
    elf="${appdir}/build/${app}.elf"
    if [ ! -f "$elf" ]; then
      eil_emit "$rf" FAIL "$app" "no ELF at build/${app}.elf"
      eil_progress "$app" FAIL
      return 0
    fi

    case "${HIL_MODE}" in
      uart_scrape | rtt_scrape) verdict_uart "$app" "$elf" "$rf" ;;
      alive) verdict_alive "$app" "$elf" "$rf" ;;
      jlink_memprobe) verdict_memprobe "$app" "$elf" "$rf" ;;
      hil_eth_tcp) verdict_eth_tcp "$app" "$elf" "$rf" ;;
    esac
    eil_progress "$app" "$(cut -d'|' -f1 "$rf" 2>/dev/null || printf '?')"
    return 0
  }

  eil_progress() { # <app> <status>
    local c="$NC"
    case "$2" in
      PASS) c="$GREEN" ;;
      FAIL) c="$RED" ;;
      SKIP) c="$YELLOW" ;;
    esac
    printf "${c}[eil]${NC} %-32s %s\n" "$1" "$2" >&2
  }

  if [ "${1:-}" = "--run-one" ]; then
    run_one "${2:?--run-one needs an app name}"
    exit 0
  fi

  ONLY=""
  MODE_FILTER=""
  LIST_ONLY=0
  JOBS=""
  while [ $# -gt 0 ]; do
    case "$1" in
      --only)
        ONLY="${2:?--only needs an app}"
        shift 2
        ;;
      --mode)
        MODE_FILTER="${2:?--mode needs a mode}"
        shift 2
        ;;
      -j | --jobs)
        JOBS="${2:?-j needs a count}"
        shift 2
        ;;
      --serial)
        JOBS=1
        shift
        ;;
      --skip-build)
        export EIL_SKIP_BUILD=1
        shift
        ;;
      --list)
        LIST_ONLY=1
        shift
        ;;
      -h | --help)
        sed -n '5,60p' "$0"
        exit 0
        ;;
      *)
        echo "Unknown arg: $1" >&2
        exit 2
        ;;
    esac
  done

  [ -n "$JOBS" ] || JOBS="${EIL_JOBS:-$(ra8_max_jobs)}"
  case "$JOBS" in
    '' | *[!0-9]*)
      echo "eil_all: -j must be a positive integer, got '$JOBS'" >&2
      exit 2
      ;;
  esac

  declare -a APPS=()
  while IFS= read -r name; do
    APPS+=("$name")
  done < <(hil_discover_apps "$HIL_DIR")
  while IFS= read -r name; do
    [ -f "${EIL_RA8P1_DIR}/${name}/hil.conf" ] || continue
    APPS+=("$name")
  done < <(hil_discover_apps "$EIL_RA8P1_DIR")
  if [ "${#APPS[@]}" -eq 0 ]; then
    echo -e "${RED}[eil_all]${NC} no apps found under ${HIL_DIR}" >&2
    exit 1
  fi

  eil_mode_of() { # <app> -> HIL_MODE on stdout (empty if no/blank hil.conf)
    local conf
    conf="$(eil_app_dir "$1")/hil.conf"
    [ -f "$conf" ] || return 0
    grep -E '^HIL_MODE=' "$conf" | head -1 | cut -d= -f2 | tr -d '"' || true
  }

  declare -a SEL=()
  for app in "${APPS[@]}"; do
    [ -n "$ONLY" ] && [ "$ONLY" != "$app" ] && continue
    if [ -n "$MODE_FILTER" ]; then
      [ "$(eil_mode_of "$app")" = "$MODE_FILTER" ] || continue
    fi
    SEL+=("$app")
  done
  if [ "${#SEL[@]}" -eq 0 ]; then
    echo -e "${RED}[eil_all]${NC} no apps match the selection" >&2
    exit 1
  fi

  if [ "$LIST_ONLY" -eq 1 ]; then
    printf '%-34s %s\n' "APP" "MODE"
    for app in "${SEL[@]}"; do
      printf '%-34s %s\n' "$app" "$(eil_mode_of "$app" || printf '(no hil.conf)')"
    done
    exit 0
  fi

  # The gate calls bare `zig` and the app builds configure through
  # cmake/toolchain-ra8d2.cmake. On the runner and in the devcontainer all of it
  # is on PATH; on a dev box it often lives in user-space installs instead (the
  # Arm toolchain under ~/opt, a zig distribution beside an older one,
  # cmake/just/ninja in ~/.local/bin). Discover and export it here -- RA8_ZIG
  # and RA8_ARM_TOOLCHAIN_BIN keep their documented precedence -- so a local
  # EIL run needs no hand-built PATH and no sudo. After --list, which must
  # stay tool-free.
  # shellcheck source=scripts/builders/find_tools.sh
  . "$REPO_ROOT/scripts/builders/find_tools.sh"
  ra8_find_tools_activate

  echo -e "${CYAN}[eil_all]${NC} ${#SEL[@]} app(s), ra8_emulator EIL, -j ${JOBS}"

  if [ -n "${RA8_EMULATOR:-}" ]; then
    EIL_EMU="$RA8_EMULATOR"
    echo -e "${CYAN}[eil_all]${NC} using prebuilt RA8_EMULATOR -> ${EIL_EMU}"
  else
    if [ ! -f "${EMU_DIR}/build.zig" ]; then
      echo -e "${RED}[eil_all]${NC} tools/ra8_emulator is a submodule (bsikar/ra8-emulator) and is not checked out." >&2
      echo "Run 'git submodule update --init tools/ra8_emulator' and retry." >&2
      exit 1
    fi
    echo -e "${CYAN}[eil_all]${NC} building ra8_emulator (zig build) ..."
    (cd "${EMU_DIR}" && zig build -Doptimize=ReleaseFast) >/tmp/eil_emu_build.log 2>&1
    EIL_EMU="${EMU_DIR}/zig-out/bin/ra8_emulator"
  fi
  if [ ! -x "$EIL_EMU" ]; then
    echo -e "${RED}[eil_all]${NC} ra8_emulator failed to build or is not executable: ${EIL_EMU} (see /tmp/eil_emu_build.log)" >&2
    exit 1
  fi

  EIL_RUN_DIR="$(mktemp -d "${TMPDIR:-/tmp}/eil_all.XXXXXX")"
  cleanup() { rm -rf "$EIL_RUN_DIR"; }
  trap cleanup EXIT

  EIL_FONT_IMG=""
  case " ${SEL[*]} " in
    *" sd_font_render "*) build_font_card ;;
  esac

  case " ${SEL[*]} " in
    *" ereader_shelf "*) build_shelf_library ;;
  esac

  export EIL_RUN_DIR EIL_EMU EIL_FONT_IMG

  echo -e "${CYAN}[eil_all]${NC} running ${#SEL[@]} ra8_emulator instance(s) ..."
  SECONDS=0
  printf '%s\n' "${SEL[@]}" |
    xargs -P "$JOBS" -I{} /bin/bash -p \
      "$REPO_ROOT/scripts/emu/eil_all.sh" --run-one {} || true
  ELAPSED=$SECONDS

  declare -i pass=0 fail=0 skip=0
  declare -a rows=()
  declare -a failed=() skipped=()
  for app in "${SEL[@]}"; do
    rf="${EIL_RUN_DIR}/${app}.result"
    if [ -f "$rf" ]; then
      line="$(cat "$rf")"
    else
      line="FAIL|${app}|unknown|worker produced no result (crash?)"
    fi
    rows+=("$line")
  done

  echo
  echo "==================================================================="
  printf '%-34s %-16s %s\n' "APP" "MODE" "RESULT"
  echo "-------------------------------------------------------------------"
  while IFS= read -r line; do
    status="${line%%|*}"
    rest="${line#*|}"
    app="${rest%%|*}"
    rest="${rest#*|}"
    mode="${rest%%|*}"
    detail="${rest#*|}"
    c="$NC"
    case "$status" in
      PASS)
        c="$GREEN"
        pass+=1
        ;;
      FAIL)
        c="$RED"
        fail+=1
        failed+=("${app} (${mode}): ${detail}")
        ;;
      SKIP)
        c="$YELLOW"
        skip+=1
        skipped+=("${app} (${mode}): ${detail}")
        ;;
    esac
    printf "%-34s %-16s ${c}%-6s${NC} %s\n" "$app" "$mode" "$status" "$detail"
  done < <(printf '%s\n' "${rows[@]}" | sort)
  echo "==================================================================="
  echo -e "  ${GREEN}${pass} passed${NC}, ${RED}${fail} failed${NC}, ${YELLOW}${skip} skipped${NC}  (${#SEL[@]} apps in ${ELAPSED}s wall, -j ${JOBS})"

  if [ "${#failed[@]}" -gt 0 ]; then
    echo
    echo -e "  ${RED}FAILED (EIL-capable modes -- ra8_emulator gaps or real regressions):${NC}"
    for a in "${failed[@]}"; do echo "    - $a"; done
  fi
  if [ "${#skipped[@]}" -gt 0 ]; then
    echo
    echo -e "  ${YELLOW}SKIPPED (hardware-only modes ra8_emulator cannot check):${NC}"
    for a in "${skipped[@]}"; do echo "    - $a"; done
  fi

  [ "$fail" -eq 0 ]
else
  [[ "$-" == *p* ]]
fi
