#!/usr/bin/env bash
#
# scripts/sim/matrix.sh -- the #67 "run every example" coverage matrix.
#
# Where board_sim_smoke.sh is a curated CI gate (deep per-app assertions on a
# hand-picked set), this is the breadth gate: it builds and boots EVERY example
# under examples/ek_ra8d2/ on the board emulator and reports, per app, whether
# it builds, boots, and runs to the budget without faulting -- the #67 success
# criterion "every example boots + exercises its peripheral in the simulator".
#
# It is intentionally a BASIC boot/exercise classifier (build / fault / halt /
# ran-clean / review); the smoke gate keeps the strong per-peripheral
# assertions. The classifier is HONEST by construction:
#
#   * The emulator build is hard-checked (configure + build + an executable
#     binary) before any app is judged -- a stale or missing board_sim aborts
#     the whole matrix instead of silently misjudging every app.
#   * Two-image TrustZone apps are DETECTED, not hand-listed: an app that links
#     a separate Non-Secure executable (an `ns_image.ld` -> `--ns` recipe)
#     cannot run through this single-image path, so it is reported SPECIAL.
#     Dual-core apps that EMBED the Cortex-M33 image (.cpu1_image, detected via
#     a `cpu1_reset_handler` export or a `linker_script_cpu1.ld`) DO run through
#     the normal path -- board_sim auto-boots the embedded cpu1 image -- so they
#     are run, not skipped, and the detection is logged so nothing is silent.
#   * An app that runs to the budget but prints neither a clean-budget marker
#     nor a fault/halt marker is UNKNOWN (needs review), never assumed OK.
#   * board_sim's exit code is captured and factored in: a non-zero exit (or a
#     124 timeout) is FAULT and can never read OK, independent of stdout text.
#   * The _unsupported tier needs external hardware and is listed as SKIPPED.
#
# Usage:
#   scripts/sim/matrix.sh                 # every ek_ra8d2 example
#   scripts/sim/matrix.sh blink dtc_transfer_demo   # explicit subset
#   BUILD_TIMEOUT=240 RUN_TIMEOUT=90 scripts/sim/matrix.sh
#
# Output: a per-app table on stdout plus a coverage summary, and a machine-
# readable report at $ROOT/build/board_sim_matrix.txt.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
# Shell options: -u (no unset vars) and -o pipefail (a failing stage fails the
# pipe). We deliberately DO NOT use -e. Like the sibling board_sim_smoke gate --
# which records fail=1 and keeps going -- this is a BREADTH report that must run
# to completion across per-app build/boot failures and tally every one of them.
# Aborting on the first non-zero (whether via -e or an aborting ERR trap) would
# stop mid-sweep and defeat the whole point of a coverage matrix, so failures
# are handled explicitly and counted instead. An informational ERR trap is also
# omitted: this gate runs many intentionally-failing probes (grep misses, `make`
# under `if !`, `[ ... ]` tests) and a trap would fire on every one of them and
# tell us nothing. With pipefail, `cmd | grep -q` would misreport when grep
# closes the pipe early (SIGPIPE on the producer); every text test below uses a
# here-string (`grep -q ... <<<"$var"`) so there is no pipe to break.
set -uo pipefail
export LC_ALL=C

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 1
sim_dir="$ROOT/tools/board_sim"
report="$ROOT/build/board_sim_matrix.txt"
mkdir -p "$ROOT/build"

build_timeout="${BUILD_TIMEOUT:-240}"
run_timeout="${RUN_TIMEOUT:-90}"

# A diagnostic line (to stderr) so detection/discovery decisions are never
# silent. stderr keeps these off the stdout per-app table, which would otherwise
# corrupt a row mid-line (the row is printed without a trailing newline until
# its verdict is known); stdout stays the clean, parseable table + summary.
log() { printf 'board_sim matrix: %s\n' "$*" >&2; }

# -- Build the emulator, and HARD-CHECK it. -----------------------------------
# If board_sim does not configure + build into a runnable binary, $sim would be
# missing or stale and EVERY app would then be misjudged against the wrong (or a
# nonexistent) emulator. Abort the whole matrix with a clear error instead.
sim_cmake_log="$ROOT/build/board_sim_matrix_emu_build.log"
log "building the emulator ..."
if ! cmake -B "$sim_dir/build" -S "$sim_dir" >"$sim_cmake_log" 2>&1; then
  echo "FATAL: board_sim cmake configure failed (see $sim_cmake_log)" >&2
  exit 2
fi
if ! cmake --build "$sim_dir/build" -j >>"$sim_cmake_log" 2>&1; then
  echo "FATAL: board_sim build failed (see $sim_cmake_log)" >&2
  exit 2
fi
sim="$sim_dir/build/board_sim"
if [ ! -x "$sim" ]; then
  echo "FATAL: board_sim binary missing or not executable: $sim" >&2
  echo "       (build reported success but produced no runnable emulator)" >&2
  exit 2
fi
log "emulator OK -> $sim"

# -- Discover apps. -----------------------------------------------------------
# Default: every main.c under the supported (ek_ra8d2) tier. The _unsupported/
# tier needs external hardware -- it is listed as SKIPPED below, never run.
#
# De-dupe by FULL directory path (not by basename) so two examples that happen
# to share a leaf name are both kept, and log any basename collision so nothing
# is dropped silently. No -maxdepth cap, so examples nested deeper than the
# current 4 levels are not silently excluded either.
skipped_apps=()
if [ "$#" -gt 0 ]; then
  mapfile -t apps < <(printf '%s\n' "$@")
else
  # Portable discovery (BSD/macOS find has no -printf): list each main.c, strip
  # the trailing /main.c to get the app dir, keep full dirs unique, then map to
  # leaf names; a duplicate leaf name is reported, never silently merged.
  mapfile -t app_dirs < <(
    find examples/ek_ra8d2 -mindepth 2 -name main.c -not -path '*/build/*' \
      -not -path '*/build-sim/*' 2>/dev/null | sed 's#/main.c$##' | sort -u
  )
  apps=()
  declare -A seen_name=()
  for d in "${app_dirs[@]}"; do
    [ -z "$d" ] && continue
    name="${d##*/}"
    if [ -n "${seen_name[$name]:-}" ]; then
      log "WARNING name collision: '$name' at both '${seen_name[$name]}' and '$d' -- both kept"
    else
      seen_name[$name]="$d"
    fi
    apps+=("$name")
  done
  # The _unsupported tier: needs external hardware, listed as SKIPPED (matching
  # the header). Discovered the same way so the report is complete.
  mapfile -t skipped_apps < <(
    find examples/_unsupported -mindepth 1 -name main.c -not -path '*/build/*' \
      -not -path '*/build-sim/*' 2>/dev/null | sed 's#/main.c$##; s#.*/##' | sort -u
  )
fi

n_total=0
n_ok=0
n_fault=0
n_halt=0
n_build=0
n_special=0
n_unknown=0
n_skipped=0
: >"$report"

# Resolve an app's source directory from its leaf name (build dirs excluded).
app_src_dir() { # name -> source dir on stdout (empty if not found)
  find examples -type d -name "$1" -not -path '*/build/*' \
    -not -path '*/build-sim/*' 2>/dev/null | head -1
}

# True if the app links a SEPARATE Non-Secure executable (a `--ns` recipe). Such
# two-image TrustZone apps cannot boot through this single-image path. The
# in-tree signal is a dedicated NS-image linker script (`ns_image.ld`); a bare
# `ns_main.c` does NOT count (some apps, e.g. cpu1_pingpong_ipc, compile it into
# the single secure image and embed cpu1, so they run fine here).
is_two_image_ns() { # src-dir -> 0 (true) if it needs --ns
  local dir="$1"
  [ -n "$dir" ] || return 1
  [ -f "$dir/ns_image.ld" ]
}

# True if the app is a dual-core build whose Cortex-M33 image is EMBEDDED in the
# single ELF (board_sim auto-boots it). Detected by a `cpu1_reset_handler`
# export in the built ELF, or a `linker_script_cpu1.ld` in the source dir.
is_dualcore_embedded() { # elf src-dir -> 0 (true) if dual-core embedded image
  local elf="$1" dir="$2" nm_syms=""
  if [ -n "$dir" ] && [ -f "$dir/linker_script_cpu1.ld" ]; then
    return 0
  fi
  if [ -n "$elf" ] && [ -f "$elf" ]; then
    nm_syms="$(arm-none-eabi-nm "$elf" 2>/dev/null || true)"
    if grep -q 'cpu1_reset_handler' <<<"$nm_syms"; then
      return 0
    fi
  fi
  return 1
}

# Classify one run from board_sim's stdout AND its exit code.
classify_run() { # out rc -> OK|FAULT|HALT|UNKNOWN
  local out="$1" rc="$2"
  # (4)(5) board_sim returns non-zero on fault / BKPT / timeout (124) and 0 only
  # on a clean run-to-budget. A non-zero exit is FAULT and can never be OK -- do
  # not trust stdout strings alone (an emulation error outside the marker set
  # below is still caught here).
  if [ "$rc" -ne 0 ]; then
    echo FAULT
    return
  fi
  # Belt-and-suspenders: a known fault marker in stdout is FAULT even if the
  # exit code did not (yet) reflect it.
  if grep -qE "INVALID INSN|UNMAPPED|executed a BKPT|FAULT|mmio_map failed" <<<"$out"; then
    echo FAULT
    return
  fi
  # rc == 0 and no fault marker: a recognised clean-budget marker is a genuine OK.
  if grep -qE "EXECUTED to the run budget|device CONFIGURED|PASS" <<<"$out"; then
    echo OK
    return
  fi
  # A firmware park / halt loop: booted, then gave up or finished a self-test.
  if grep -qE "parked|halt loop|reached .*_halt" <<<"$out"; then
    echo HALT
    return
  fi
  # (3) Ran to here (rc==0) with neither a clean-budget nor a halt marker: do
  # NOT assume OK. Tag UNKNOWN for human review so a broken example -- one that
  # boots but produces no recognised evidence -- cannot read green.
  echo UNKNOWN
}

# -- Per-app matrix. ----------------------------------------------------------
for app in "${apps[@]}"; do
  [ -z "$app" ] && continue
  n_total=$((n_total + 1))
  printf '  %-26s ' "$app"

  src_dir="$(app_src_dir "$app")"

  # Two-image TrustZone (--ns) apps need their dedicated recipe -> SPECIAL.
  if is_two_image_ns "$src_dir"; then
    echo "SPECIAL (two-image TrustZone -- needs --ns; detected ns_image.ld)"
    printf '%-26s SPECIAL\n' "$app" >>"$report"
    n_special=$((n_special + 1))
    continue
  fi

  if ! timeout "$build_timeout" make "$app" >"/tmp/matrix_build_$app.log" 2>&1; then
    echo "BUILD FAIL (see /tmp/matrix_build_$app.log)"
    printf '%-26s BUILD_FAIL\n' "$app" >>"$report"
    n_build=$((n_build + 1))
    continue
  fi
  elf="$(find examples -path "*/$app/build/$app.elf" 2>/dev/null | head -1)"
  if [ -z "$elf" ]; then
    echo "NO ELF"
    printf '%-26s NO_ELF\n' "$app" >>"$report"
    n_build=$((n_build + 1))
    continue
  fi

  # Dual-core embedded-image apps run through the normal path (board_sim boots
  # the embedded cpu1 image). Detect + log it so nothing is silently special.
  note=""
  if is_dualcore_embedded "$elf" "$src_dir"; then
    note=" [dual-core embedded cpu1 image]"
    log "detected dual-core embedded cpu1 image in '$app' -- running via single-image path"
  fi

  out="$(BOARD_SIM_MAX_CHUNKS=4000 BOARD_SIM_WALL_S=60 \
    timeout "$run_timeout" "$sim" "$elf" 2>&1)"
  rc=$?
  verdict="$(classify_run "$out" "$rc")"
  case "$verdict" in
    OK)
      echo "OK (boots + runs to budget)$note"
      n_ok=$((n_ok + 1))
      ;;
    FAULT)
      echo "FAULT (rc=$rc -- board_sim model gap or firmware bug)$note"
      n_fault=$((n_fault + 1))
      ;;
    HALT)
      echo "HALT (parked -- self-test done or panic)$note"
      n_halt=$((n_halt + 1))
      ;;
    UNKNOWN)
      echo "UNKNOWN (ran to budget, no clean/halt marker -- REVIEW)$note"
      n_unknown=$((n_unknown + 1))
      ;;
  esac
  printf '%-26s %s\n' "$app" "$verdict" >>"$report"
done

# -- The _unsupported tier: listed as SKIPPED, never built or run. ------------
for app in "${skipped_apps[@]}"; do
  [ -z "$app" ] && continue
  n_skipped=$((n_skipped + 1))
  printf '  %-26s SKIPPED (_unsupported tier -- needs external hardware)\n' "$app"
  printf '%-26s SKIPPED\n' "$app" >>"$report"
done

# -- Honest summary. ----------------------------------------------------------
# runnable = apps we actually attempt to boot (total minus the SPECIAL two-image
# apps). SKIPPED (_unsupported) are not part of n_total. The headline coverage
# counts ONLY genuinely-verified OK runs (a clean run-to-budget); HALT, UNKNOWN,
# FAULT and BUILD failures are all reported explicitly so the number is honest.
runnable=$((n_total - n_special))
echo ""
echo "board_sim matrix coverage:"
echo "  total examples : $n_total"
echo "  special (--ns) : $n_special  (two-image TrustZone -- dedicated recipe)"
echo "  skipped (unsup): $n_skipped  (_unsupported tier -- external hardware)"
echo "  build failed   : $n_build"
echo "  faulted        : $n_fault"
echo "  halted/parked  : $n_halt"
echo "  unknown/review : $n_unknown"
echo "  booted OK      : $n_ok"
if [ "$runnable" -gt 0 ]; then
  pct=$((n_ok * 100 / runnable))
  echo "  boot coverage  : ${pct}% verified-OK of runnable (${n_ok}/${runnable})"
fi
echo "  report written : $report"

# Non-zero exit if anything build-failed, faulted, or is UNKNOWN (not verified),
# so this can gate CI later. SPECIAL and SKIPPED do not fail the gate; HALT
# booted to budget and does not fail it either.
[ "$n_build" -eq 0 ] && [ "$n_fault" -eq 0 ] && [ "$n_unknown" -eq 0 ]
