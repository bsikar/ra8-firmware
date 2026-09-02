#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
#
# eil_all.sh -- EIL (emulator-in-the-loop): run every HIL app in the
# tools/ra8_emulator HEADLESS and verify the SAME per-app hil.conf
# expectations the real-hardware suite (scripts/hil/all.sh) checks -- with NO
# board attached. It is the hardware-free mirror of hil_all.sh, fast enough to
# gate every push in CI (a ra8_emulator run is seconds; the whole set runs in
# parallel). A ra8_emulator regression -- e.g. the div-0 that slipped in recently
# -- fails here instantly instead of only showing up on the bench.
#
# For each app under examples/ek_ra8d2/hw_validated/hil/ -- plus any RA8P1/NPU
# foundation app under examples/ra8p1_foundation/ that carries a hil.conf, which
# ra8_emulator emulates via `--device ra8p1` (declared per app as HIL_EMU_ARGS) --
# eil_all.sh sources the same hil.conf (via scripts/hil/lib/hil_conf.sh, shared with
# hil_all.sh) and dispatches by HIL_MODE:
#
#   uart_scrape / alive   Build the .elf, boot it in ra8_emulator headless, capture
#                         the emulated SCI UART, and PASS iff HIL_EXPECT appears
#                         AND (if set) HIL_EXPECT_NEGATIVE does NOT, AND the run
#                         did not fault (no invalid-opcode / unmapped access) and
#                         reached its run budget. Mirrors scripts/emu/smoke.sh's
#                         invocation + UART capture so behaviour matches.
#
#   jlink_memprobe        On hardware this reads a free-running counter over
#                         J-Link. In ra8_emulator the equivalent is `--dump-sym`
#                         (read a 32-bit global after the run) plus the
#                         `--stop-sym <sym> <N>` early-stop this suite added:
#                         run until HIL_PROBE_SYMBOL reaches HIL_PROBE_MIN_ADVANCE
#                         (the emulator starts every counter at 0 on reset, so the
#                         final value IS the delta the HIL probe measures), then
#                         PASS iff it did AND the optional HIL_PROBE_FAILURE_SYMBOL
#                         stayed <= HIL_PROBE_MAX_FAILURE AND the run did not
#                         fault. An app whose counter never advances (ra8_emulator
#                         does not model that peripheral) FAILS -- a real,
#                         reported modelling gap, not a false pass.
#
#   rtt_scrape            Checked exactly like uart_scrape: ra8_emulator models
#                         the SEGGER RTT host side (board_periph_rtt.c scans
#                         SRAM for the "SEGGER RTT" control block and drains
#                         up-buffer 0, echoing each line as `[rtt] ...` on the
#                         same console output STOP_ON watches), so the RTT
#                         banner is asserted with no J-Link attached.
#   hil_eth_tcp           On hardware scripts/hil/eth_tcp.sh probes the wire
#                         from the Pi: send N random bytes over TCP, assert a
#                         byte-exact echo. ra8_emulator ships that peer IN-PROCESS
#                         (board_net, fed by the register-level R-Switch model):
#                         it resolves the firmware over ARP, pings it, opens TCP
#                         to the echo port, sends a payload, and byte-verifies
#                         the echo. PASS iff the firmware's served-echo banner
#                         (HIL_EXPECT) shows on the UART AND the peer's NET TCP
#                         report says "echo MATCH" AND the run did not fault.
#
# A SKIP is a clearly-reported "ra8_emulator cannot check this mode" -- never a
# false pass. Only apps in an EIL-capable mode (uart_scrape / alive /
# jlink_memprobe / rtt_scrape / hil_eth_tcp) can FAIL the suite; SKIPs do not.
#
# EIL==HIL discipline (enforced by scripts/checks/check_hil_eil_parity.py):
# "EIL==HIL" means ra8_emulator must reproduce the ACTUAL silicon behaviour of
# every HIL path -- bugs INCLUDED. There must be NO EIL skips: when a new HIL app
# exercises a peripheral/path ra8_emulator does not yet model, ra8_emulator MUST be
# extended (and a new EIL-capable mode added here if needed) so EIL stays
# complete -- the gate makes skipping impossible, forcing the emulator to keep up with
# the hardware. That gate re-derives this suite's app discovery + EIL-capable
# mode list, so an added HIL app cannot silently escape EIL coverage.
#
# PARALLELISM: unlike hil_all.sh (serial -- one physical board), ra8_emulator
# instances share no hardware, so apps run CONCURRENTLY in a worker pool of
# EIL_JOBS (default: the bounded canonical width ra8_max_jobs; -j N overrides).
# Each app builds into its OWN isolated per-app build dir and runs its OWN
# ra8_emulator process writing its OWN result file, so concurrent runs never
# collide. The per-app PASS/FAIL verdict is independent of -j, so the final
# (sorted) matrix is deterministic.
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
#   EIL_JOBS               parallel workers (default: ra8_max_jobs -- #328;
#                          RA8_MAX_JOBS / CMAKE_BUILD_PARALLEL_LEVEL / nproc)
#   EIL_UART_MAX_CHUNKS    uart_scrape/alive instruction-chunk cap (150000; a
#                          hil.conf may raise its own via HIL_EMU_MAX_CHUNKS)
#   EIL_UART_WALL_S        uart_scrape/alive CPU-time guard, s (0=off; default 0
#                          -- keep off so verdicts stay -j-independent)
#   EIL_PROBE_MAX_CHUNKS   jlink_memprobe chunk cap (150000)
#   EIL_PROBE_WALL_S       jlink_memprobe CPU-time guard, s (0=off; default 0)
#
# Exit: 0 = every EIL-capable app passed; 1 = at least one failed; 2 = usage.
#
# Designed to be the one hardware-free entry-point for both local dev and CI.

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
  # EIL also covers RA8P1/NPU foundation apps that carry a hil.conf. ra8_emulator
  # emulates the RA8P1 part (--device ra8p1, declared per app via HIL_EMU_ARGS);
  # there is no RA8P1 HIL rig, so these are discovered HERE but never by
  # hil_all.sh (which would try to flash them to the RA8D2 board).
  EIL_RA8P1_DIR="${REPO_ROOT}/examples/ra8p1_foundation"
  EMU_DIR="${REPO_ROOT}/tools/ra8_emulator"

  # ra8_max_jobs -- the ONE canonical bounded-parallelism width (#328). The
  # ra8_emulator worker pool and the emulator build below derive from it so this
  # sweep does not saturate a shared box (the contention that timed out a
  # emulator gate and read as a regression).
  # shellcheck source=scripts/ci/lib/parallelism.sh
  source "${REPO_ROOT}/scripts/ci/lib/parallelism.sh"

  # ra8_emulator is C23 (typed enums, nullptr) and links C++ TUs, so its build must
  # pin a C23-capable C/C++ pair -- the ambient "cc" on the Debian 12 dev box is
  # gcc 12 and rejects the syntax outright (#467). Reuse the ONE shared selector
  # the host-test and coverage builds use rather than hand-rolling a second probe.
  # shellcheck source=scripts/builders/select_host_compiler.sh
  source "${REPO_ROOT}/scripts/builders/select_host_compiler.sh"

  # Shared app discovery + hil.conf sourcing (also used by scripts/hil/all.sh).
  # shellcheck source=scripts/hil/lib/hil_conf.sh
  source "${REPO_ROOT}/scripts/hil/lib/hil_conf.sh"

  # ra8_emulator's run log can carry non-UTF-8 bytes (register dumps, SD/SPI noise);
  # force the C locale so grep/sed treat input as raw bytes on macOS too.
  export LC_ALL=C

  GREEN='\033[0;32m'
  RED='\033[0;31m'
  YELLOW='\033[1;33m'
  CYAN='\033[0;36m'
  NC='\033[0m'

  # Run budgets. STOP_ON (uart) / --stop-sym (memprobe) end a PASSING app in a
  # handful of chunks; MAX_CHUNKS is the DETERMINISTIC, instruction-counted
  # backstop for an app that never reaches its expectation.
  #
  # The wall-clock guard is disabled by default (WALL_S=0). ra8_emulator's guard is
  # CPU-time (clock()), which under N-way parallelism inflates for a fixed
  # instruction count (N Unicorn JITs contend for cache/memory, so each emulated
  # chunk burns more CPU-seconds). A ThreadX/USBX app that reaches its banner at a
  # low chunk count can therefore breach a finite CPU-time budget under -j but not
  # serially -- flipping its verdict with the parallelism, which must NOT happen
  # (the #168 lesson, sharpened). Bounding purely by the instruction-counted
  # MAX_CHUNKS makes every verdict a function of the deterministic instruction
  # stream alone -- identical serial or at any -j. Set EIL_*_WALL_S>0 to re-arm the
  # guard (useful only to cap a pathological run locally; it forfeits -j
  # determinism).
  #
  # The default cap is deliberately LOW (150000). STOP_ON / --stop-sym end a
  # passing app long before it, so the cap only bounds a GENUINELY-STUCK app --
  # and a low cap makes such a failure fail FAST (a spinning ThreadX/USBX app that
  # never reaches its banner is expensive to emulate per chunk). The handful of
  # compute-heavy apps whose banner legitimately lands past 150000 chunks (e.g. the
  # software-crypto KAT in psa_crypto_hil at chunk ~220k) raise their own cap with
  # HIL_EMU_MAX_CHUNKS in their hil.conf, so the global cap need not carry them.
  EIL_UART_MAX_CHUNKS="${EIL_UART_MAX_CHUNKS:-150000}"
  EIL_UART_WALL_S="${EIL_UART_WALL_S:-0}"
  EIL_PROBE_MAX_CHUNKS="${EIL_PROBE_MAX_CHUNKS:-150000}"
  EIL_PROBE_WALL_S="${EIL_PROBE_WALL_S:-0}"

  # -----------------------------------------------------------------------------
  # Per-app ra8_emulator extra arguments -- emu_extra_args() lives in
  # scripts/emu/emu_fixtures.sh, shared with scripts/emu/matrix.sh so that "this
  # app needs a card" is recorded in exactly one place. It used to live here
  # alone, and the breadth matrix consequently booted card-dependent apps with no
  # card and blamed them for it.
  # -----------------------------------------------------------------------------
  # shellcheck source=scripts/emu/emu_fixtures.sh
  . "${REPO_ROOT}/scripts/emu/emu_fixtures.sh"

  # Per-app instruction-chunk cap override for the few compute-heavy apps whose
  # legitimate banner lands past the low global cap. Kept here (not in the app's
  # HIL manifest) alongside emu_extra_args; a hil.conf may still declare its own
  # HIL_EMU_MAX_CHUNKS, which wins over this default. Empty = use the global cap.
  eil_app_max_chunks() { # <app> -> chunk cap on stdout (may be empty)
    case "$1" in
      psa_crypto_hil) printf '320000' ;; # software-crypto KAT banner at chunk ~220k
      *) : ;;
    esac
  }

  # Bake a FAT card carrying FONT.OTF once (for sd_font_render, which reads a font
  # off the card rather than provisioning one). ra8_emulator opens a --sd image
  # read-only into memory, so every parallel worker can share this one file
  # safely. Sets EIL_FONT_IMG under EIL_RUN_DIR (cleaned up with it); leaves it
  # empty -- so the app fails with a precise "no FONT.OTF card" reason -- when
  # mkfontimg or the font fixture is unavailable.
  build_font_card() {
    local font="${REPO_ROOT}/apps/shared_libs/third_party/litehtml/containers/test/fonts/ahem.ttf"
    local mk="${REPO_ROOT}/tools/mkfontimg"
    [ -f "$font" ] || return 0
    cmake -B "${mk}/build" -S "$mk" >/dev/null 2>&1 || return 0
    cmake --build "${mk}/build" >/dev/null 2>&1 || return 0
    EIL_FONT_IMG="${EIL_RUN_DIR}/font.img"
    "${mk}/build/mkfontimg" "$font" "$EIL_FONT_IMG" FONT.OTF >/dev/null 2>&1 || EIL_FONT_IMG=""
  }

  # Ensure ereader_shelf's baked MRAM library header (library.h) exists before the
  # app is built. That header is a generated, gitignored artifact derived from
  # three Git-LFS epub sources: on a clean checkout it is absent, so the app fails
  # to COMPILE (a build failure that is not a ra8_emulator modelling gap). Regenerate
  # just that three-book subset here -- `build_books.sh --shelf-only` is the fast
  # path (three epubs, no full-library compile or manifest) -- so the EIL build has
  # it. Only when the header is MISSING: an existing header (and the golden
  # framebuffer hash the banner pins to its exact baked pixels) is left untouched.
  # If the epub sources or Pillow are unavailable the generation fails, the header
  # stays absent, and the worker reports a plain build failure -- never a false
  # pass.
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

  # Resolve an app name to its source directory. App names are unique across the
  # two EIL roots (the ek_ra8d2 hil/ suite and the RA8P1 foundation), so a simple
  # "hil/ first, else RA8P1" lookup is unambiguous.
  eil_app_dir() { # <app> -> app directory on stdout
    if [ -d "${HIL_DIR}/$1" ]; then
      printf '%s' "${HIL_DIR}/$1"
    else
      printf '%s' "${EIL_RA8P1_DIR}/$1"
    fi
  }

  # Two-image TrustZone support: a build that also produced <app>_ns.elf next to
  # the Secure <app>.elf (e.g. tz_nsc_cgc_usb) is a two-project TrustZone app.
  # On hardware the two halves are merged into one flashable hex; ra8_emulator
  # instead takes the Non-Secure ELF via --ns, loads it at its LMA, and resolves
  # probe symbols from BOTH symbol tables -- without it the Secure boot BLXNS-es
  # into an empty NS alias and every NS-side counter reads as unresolved.
  eil_ns_elf_args() { # <secure-elf> -> "--ns <ns-elf>" on stdout (or empty)
    # NB: must exit 0 either way -- the callers expand this inside an assignment
    # under `set -e`, where a bare failing [ -f ] test would abort the worker.
    local ns="${1%.elf}_ns.elf"
    if [ -f "$ns" ]; then
      printf -- '--ns %s' "$ns"
    fi
  }

  # =============================================================================
  # Worker verdict helpers -- run inside the `--run-one <app>` subprocess, which
  # has already sourced this file (so budgets + helpers are defined) and loaded
  # the app's hil.conf (so HIL_* are set). Each writes exactly one result line
  # "<STATUS>|<app>|<mode>|<detail>" to the app's result file; STATUS is
  # PASS / FAIL / SKIP and <detail> never contains a '|'.
  # =============================================================================

  # The three ra8_emulator give-up signatures a run must NOT contain.
  eil_fault_line() { # <captured-output> -> nonempty fault description on stdout
    local out="$1"
    if grep -qE 'INVALID INSN|UNMAPPED' <<<"$out"; then
      printf 'fault: invalid opcode / unmapped access'
    elif grep -q 'executed a BKPT' <<<"$out"; then
      printf 'fault: BKPT halt (assert / give-up)'
    fi
  }

  # uart_scrape + alive: boot headless, capture the emulated UART, assert the
  # hil.conf expectation exactly as scripts/emu/smoke.sh's default path does.
  verdict_uart() { # <app> <elf> <result_file>
    local app="$1" elf="$2" rf="$3"
    local expect="${HIL_EXPECT:-}" neg="${HIL_EXPECT_NEGATIVE:-}"
    local extra out fault
    if [ -z "$expect" ]; then
      eil_emit "$rf" FAIL "$app" "hil.conf has no HIL_EXPECT"
      return
    fi
    # emu_extra_args is the harness's per-app device knowledge (cards); HIL_EMU_ARGS
    # is the app's own manifest-declared ra8_emulator flags (e.g. --device ra8p1).
    extra=()
    read -r -a extra <<<"$(emu_extra_args "$app") $(eil_ns_elf_args "$elf") ${HIL_EMU_ARGS:-}"
    out="$(RA8_EMU_STOP_ON="$expect" RA8_EMU_WALL_S="$EIL_UART_WALL_S" \
      RA8_EMU_MAX_CHUNKS="${HIL_EMU_MAX_CHUNKS:-$EIL_UART_MAX_CHUNKS}" \
      "$EIL_EMU" "$elf" ${extra[@]+"${extra[@]}"} 2>&1 || true)"
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
      if grep -q 'TRUNCATED by the wall-clock guard' <<<"$out"; then
        eil_emit "$rf" FAIL "$app" "no banner; run truncated at ${EIL_UART_WALL_S}s (raise EIL_UART_WALL_S?)"
      else
        eil_emit "$rf" FAIL "$app" "UART lacks '${expect}' (ra8_emulator gap?)"
      fi
      return
    fi
    if ! grep -q 'EXECUTED to the run budget' <<<"$out"; then
      eil_emit "$rf" FAIL "$app" "banner seen but run did not reach budget"
      return
    fi
    eil_emit "$rf" PASS "$app" "uart '${expect}'"
  }

  # hil_eth_tcp: on hardware scripts/hil/eth_tcp.sh probes the wire from the Pi
  # (send N random bytes over TCP, read them back, assert byte-exact equality).
  # ra8_emulator ships that peer in-process: board_net -- fed frames by the
  # register-level R-Switch model in board_periph_eth.c -- resolves the firmware
  # over ARP, pings it, TCP-connects to the echo port, sends its payload, and
  # byte-verifies the echo, reporting "echo MATCH" in its end-of-run NET TCP
  # summary. The verdict therefore asserts BOTH ends of the exchange: the
  # firmware's served-echo UART banner (HIL_EXPECT, also the STOP_ON so a
  # passing run ends fast) and the peer's byte-exact MATCH verdict.
  verdict_eth_tcp() { # <app> <elf> <result_file>
    local app="$1" elf="$2" rf="$3"
    local expect="${HIL_EXPECT:-}" neg="${HIL_EXPECT_NEGATIVE:-}"
    local peer_ok="echo MATCH"
    local extra out fault
    if [ -z "$expect" ]; then
      eil_emit "$rf" FAIL "$app" "hil.conf has no HIL_EXPECT"
      return
    fi
    # emu_extra_args is the harness's per-app device knowledge (cards); HIL_EMU_ARGS
    # is the app's own manifest-declared ra8_emulator flags (e.g. --device ra8p1).
    extra=()
    read -r -a extra <<<"$(emu_extra_args "$app") $(eil_ns_elf_args "$elf") ${HIL_EMU_ARGS:-}"
    out="$(RA8_EMU_STOP_ON="$expect" RA8_EMU_WALL_S="$EIL_UART_WALL_S" \
      RA8_EMU_MAX_CHUNKS="${HIL_EMU_MAX_CHUNKS:-$EIL_UART_MAX_CHUNKS}" \
      "$EIL_EMU" "$elf" ${extra[@]+"${extra[@]}"} 2>&1 || true)"
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
    if ! grep -q 'EXECUTED to the run budget' <<<"$out"; then
      eil_emit "$rf" FAIL "$app" "banner seen but run did not reach budget"
      return
    fi
    eil_emit "$rf" PASS "$app" "uart '${expect}' + peer byte-exact echo MATCH"
  }

  # alive: liveness apps that need not print a specific banner (an MPU/fault-
  # recovery demo, a bring-up that just has to keep running). On hardware the
  # probe checks the PC/cycle counter still advances; in ra8_emulator the analog is
  # "ran without a give-up fault and did not print a failure banner". HIL_EXPECT
  # is optional here (asserted only if the manifest sets one); HIL_EXPECT_NEGATIVE
  # is asserted if present. Some alive apps deliberately raise a *recoverable*
  # fault (HIL_FAULT_EXPECTED=1) -- ra8_emulator delivers + recovers it as the real
  # core would, so only a genuine give-up (BKPT / unmapped access) counts as dead.
  verdict_alive() { # <app> <elf> <result_file>
    local app="$1" elf="$2" rf="$3"
    local expect="${HIL_EXPECT:-}" neg="${HIL_EXPECT_NEGATIVE:-}"
    local extra out fault
    # emu_extra_args is the harness's per-app device knowledge (cards); HIL_EMU_ARGS
    # is the app's own manifest-declared ra8_emulator flags (e.g. --device ra8p1).
    extra=()
    read -r -a extra <<<"$(emu_extra_args "$app") $(eil_ns_elf_args "$elf") ${HIL_EMU_ARGS:-}"
    # An empty RA8_EMU_STOP_ON is treated as "disabled" by ra8_emulator, so this
    # both STOP_ONs a named banner (fast) and runs to the budget when none is set.
    out="$(RA8_EMU_STOP_ON="$expect" RA8_EMU_WALL_S="$EIL_UART_WALL_S" \
      RA8_EMU_MAX_CHUNKS="${HIL_EMU_MAX_CHUNKS:-$EIL_UART_MAX_CHUNKS}" \
      "$EIL_EMU" "$elf" ${extra[@]+"${extra[@]}"} 2>&1 || true)"
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

  # jlink_memprobe: run until the progress counter reaches its floor (--stop-sym),
  # then read it back (--dump-sym). The emulator resets every counter to 0, so the final
  # value is the advance the HIL probe measures over its window.
  verdict_memprobe() { # <app> <elf> <result_file>
    local app="$1" elf="$2" rf="$3"
    local sym="${HIL_PROBE_SYMBOL:-}" min="${HIL_PROBE_MIN_ADVANCE:-4}"
    local fsym="${HIL_PROBE_FAILURE_SYMBOL:-}" fmax="${HIL_PROBE_MAX_FAILURE:-0}"
    local -a args=()
    local extra out fault val fval
    if [ -z "$sym" ]; then
      eil_emit "$rf" FAIL "$app" "hil.conf has no HIL_PROBE_SYMBOL"
      return
    fi
    args=(--dump-sym "$sym" --stop-sym "$sym" "$min")
    if [ -n "$fsym" ]; then
      args+=(--dump-sym "$fsym")
    fi
    # A memprobe app can still need a card attached (e.g. epub_open / pagecache
    # self-provision a book onto a blank --sd-new card before the counter moves).
    # emu_extra_args is the harness's per-app device knowledge (cards); HIL_EMU_ARGS
    # is the app's own manifest-declared ra8_emulator flags (e.g. --device ra8p1).
    extra=()
    read -r -a extra <<<"$(emu_extra_args "$app") $(eil_ns_elf_args "$elf") ${HIL_EMU_ARGS:-}"
    out="$(RA8_EMU_WALL_S="$EIL_PROBE_WALL_S" \
      RA8_EMU_MAX_CHUNKS="${HIL_EMU_MAX_CHUNKS:-$EIL_PROBE_MAX_CHUNKS}" \
      "$EIL_EMU" "$elf" "${args[@]}" ${extra[@]+"${extra[@]}"} 2>&1 || true)"
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

  # Extract the decimal value ra8_emulator's --dump-sym printed for a given symbol:
  #   "  dump-sym      : g_foo @0x2200 = 42 (0x0000002A)"  -> "42"
  # Empty if the symbol was <unresolved> / <unreadable> / absent.
  eil_dump_sym_value() { # <captured-output> <symbol> -> decimal value on stdout
    sed -n "s/.*dump-sym[[:space:]]*: $2 @0x[0-9A-Fa-f]* = \([0-9][0-9]*\) .*/\1/p" <<<"$1" | head -1
  }

  # Write one result line for an app (atomically: a single line < PIPE_BUF).
  eil_emit() { # <result_file> <status> <app> <detail>
    printf '%s|%s|%s|%s\n' "$2" "$3" "${HIL_MODE:-unknown}" "$4" >"$1"
  }

  # -----------------------------------------------------------------------------
  # --run-one <app>: the parallel worker. Build the app into its isolated per-app
  # build dir, then dispatch to the mode verdict. Always writes a result file and
  # exits 0 (the PASS/FAIL/SKIP verdict lives in the file, not the exit code) so a
  # failing app never aborts the xargs pool.
  # -----------------------------------------------------------------------------
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
    # Apply the built-in per-app chunk-cap override unless the manifest set one.
    : "${HIL_EMU_MAX_CHUNKS:=$(eil_app_max_chunks "$app")}"

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
      # rtt_scrape shares the uart verdict: ra8_emulator's RTT model drains the
      # in-RAM control block onto the same console output path ([rtt] lines),
      # so the banner grep + STOP_ON early-stop work identically to a UART app.
      uart_scrape | rtt_scrape) verdict_uart "$app" "$elf" "$rf" ;;
      alive) verdict_alive "$app" "$elf" "$rf" ;;
      jlink_memprobe) verdict_memprobe "$app" "$elf" "$rf" ;;
      hil_eth_tcp) verdict_eth_tcp "$app" "$elf" "$rf" ;;
    esac
    eil_progress "$app" "$(cut -d'|' -f1 "$rf" 2>/dev/null || printf '?')"
    return 0
  }

  # One compact colored progress line to stderr as each worker finishes (these may
  # interleave under -j, but each is a whole line; the deterministic matrix is
  # printed by the parent from the result files, not from these).
  eil_progress() { # <app> <status>
    local c="$NC"
    case "$2" in
      PASS) c="$GREEN" ;;
      FAIL) c="$RED" ;;
      SKIP) c="$YELLOW" ;;
    esac
    printf "${c}[eil]${NC} %-32s %s\n" "$1" "$2" >&2
  }

  # =============================================================================
  # Worker entry: if invoked as `--run-one <app>`, do just that app and exit.
  # =============================================================================
  if [ "${1:-}" = "--run-one" ]; then
    run_one "${2:?--run-one needs an app name}"
    exit 0
  fi

  # =============================================================================
  # Parent: parse args, discover apps, build the emulator, run the pool, report.
  # =============================================================================
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
        sed -n '5,86p' "$0"
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

  # Discover apps: the ek_ra8d2 hil/ suite (shared with hil_all.sh via hil_conf.sh)
  # plus any RA8P1 foundation app that declares a hil.conf (ra8_emulator can emulate
  # the RA8P1 via --device ra8p1). The RA8P1 apps are EIL-only -- hil_all.sh never
  # sees them, so it cannot flash an RA8P1 image to the RA8D2 board.
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

  # Resolve each selected app's mode once (for --list and the mode filter).
  eil_mode_of() { # <app> -> HIL_MODE on stdout (empty if no/blank hil.conf)
    local conf
    conf="$(eil_app_dir "$1")/hil.conf"
    [ -f "$conf" ] || return 0
    grep -E '^HIL_MODE=' "$conf" | head -1 | cut -d= -f2 | tr -d '"' || true
  }

  # Build the selected list honouring --only / --mode.
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

  echo -e "${CYAN}[eil_all]${NC} ${#SEL[@]} app(s), ra8_emulator EIL, -j ${JOBS}"

  # Build the emulator ONCE; the workers only invoke the prebuilt binary.
  echo -e "${CYAN}[eil_all]${NC} building ra8_emulator ..."
  # Pin a C23-capable host compiler (gcc-first to match CI; clang fallback where
  # the host gcc is too old). Honours a pre-set CC/CXX and, under `set -e`, aborts
  # loudly if no candidate is C23-capable -- never a silent fall-through to a
  # too-old cc.
  ra8_select_emulator_compiler
  ra8_cmake_reset_if_incompatible "${EMU_DIR}/build" "$EMU_DIR"
  cmake -B "${EMU_DIR}/build" -S "${EMU_DIR}" \
    -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" >/tmp/eil_emu_cmake.log 2>&1
  cmake --build "${EMU_DIR}/build" -j "$(ra8_max_jobs)" >/tmp/eil_emu_build.log 2>&1
  EIL_EMU="${EMU_DIR}/build/ra8_emulator"
  if [ ! -x "$EIL_EMU" ]; then
    echo -e "${RED}[eil_all]${NC} ra8_emulator failed to build (see /tmp/eil_emu_build.log)" >&2
    exit 1
  fi

  # Per-run scratch dir for the workers' result + build logs.
  EIL_RUN_DIR="$(mktemp -d "${TMPDIR:-/tmp}/eil_all.XXXXXX")"
  cleanup() { rm -rf "$EIL_RUN_DIR"; }
  trap cleanup EXIT

  # Bake the shared FONT.OTF card once if the app that reads it is selected; all
  # workers share the read-only image. Empty if unbaked (the app fails clearly).
  EIL_FONT_IMG=""
  case " ${SEL[*]} " in
    *" sd_font_render "*) build_font_card ;;
  esac

  # Generate ereader_shelf's baked library.h before the pool builds it, if that
  # app is selected and the (gitignored) header is missing. See build_shelf_library.
  case " ${SEL[*]} " in
    *" ereader_shelf "*) build_shelf_library ;;
  esac

  export EIL_RUN_DIR EIL_EMU EIL_FONT_IMG

  # Run the pool: one ra8_emulator worker per app, EIL_JOBS at a time. Each worker
  # re-execs this script as `--run-one <app>` (a fully isolated process with its
  # own build dir, emulator instance, and result file), so nothing is shared and the
  # verdict is independent of how many run at once. `|| true` keeps a crashed
  # worker from aborting the aggregation -- a missing result is caught below.
  echo -e "${CYAN}[eil_all]${NC} running ${#SEL[@]} ra8_emulator instance(s) ..."
  SECONDS=0
  printf '%s\n' "${SEL[@]}" |
    xargs -P "$JOBS" -I{} /bin/bash -p \
      "$REPO_ROOT/scripts/emu/eil_all.sh" --run-one {} || true
  ELAPSED=$SECONDS

  # Aggregate: read each app's result file (a missing one = a crashed worker),
  # sort by app name for a deterministic matrix regardless of -j.
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
