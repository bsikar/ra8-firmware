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
#   * board_sim's exit code is captured and factored in: a fault exit can never
#     read OK, independent of stdout text.
#   * A run cut short by a WALL-CLOCK bound is TRUNCATED -- a distinct third
#     state, never folded into OK or FAULT. See the determinism note below.
#   * The _unsupported tier needs external hardware and is listed as SKIPPED.
#
# DETERMINISM (#394). This matrix is a RATCHETED GATE, so the same ELF must
# yield the same verdict on an idle box and on a loaded one. Two wall-clock
# bounds used to leak host load into the verdict:
#
#   1. board_sim's own CPU-time guard (BOARD_SIM_WALL_S, a clock() budget).
#      Measured on usb_printer_vendor, three back-to-back runs of ONE binary
#      truncated at chunk 2473, 3237 and 3460 of 4000 -- a 40% spread with
#      identical UART output. Whether the app reached its budget (OK) or was
#      cut short therefore tracked how busy the host was, and the app flipped
#      OK -> FAULT across a change that touched nothing it uses. This is the
#      #168 mechanism exactly: Unicorn's TCG re-translation burns CPU-time
#      under load, so the guard fires earlier the busier the box is.
#   2. The outer `timeout` on the board_sim process, likewise wall-clock.
#
# The instruction-counted chunk budget is the deterministic bound, so the runs
# below set BOARD_SIM_WALL_S=0 -- which truly DISABLES the CPU-time guard
# (#168) -- and are bounded by chunks alone. The outer `timeout` stays only as
# a hang backstop and is sized so it is never the effective bound.
#
# Because a wall-clock stop can no longer be mistaken for a result, a run that
# hits one is reported TRUNCATED rather than guessed at. An app that did not
# reach its budget has produced NO VERDICT, and saying so is the honest output;
# folding it into FAULT invents a failure, and folding it into OK invents a
# pass. That is the touch_demo empty-verdict lesson and the #168 lesson both.
#
# Usage:
#   scripts/sim/matrix.sh                 # every ek_ra8d2 example, -j = cores
#   scripts/sim/matrix.sh blink dtc_transfer_demo   # explicit subset
#   scripts/sim/matrix.sh --selftest      # assert the classifier, build nothing
#   MATRIX_JOBS=1 scripts/sim/matrix.sh   # serial (identical verdicts, slower)
#   BUILD_TIMEOUT=240 RUN_TIMEOUT=600 MAX_CHUNKS=4000 scripts/sim/matrix.sh
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
# The outer run backstop. Deliberately generous: with the CPU-time guard off
# (see the DETERMINISM note) the chunk budget always terminates the run, so this
# exists only to catch a pathological hang. Sizing it tight would put a
# wall-clock bound back in charge of the verdict, which is the bug being fixed.
run_timeout="${RUN_TIMEOUT:-600}"
# The deterministic bound: outer chunks, one SysTick each. Instruction-counted,
# so it is identical on an idle box and a loaded one.
max_chunks="${MAX_CHUNKS:-4000}"

# Parallel workers. The sweep builds and boots ~200 images; serially that is
# hours, which is not a per-push gate. Each app is an independent build dir and
# an independent board_sim process, and -- because verdicts are bounded by the
# instruction-counted chunk budget rather than by CPU-time -- the verdict is
# identical at any -j. Parallelism is therefore free of the usual risk here:
# it can change the wall time and nothing else.
detect_jobs() { # -> host core count, or 4 when it cannot be determined
  local n=""
  n="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || printf '')"
  case "$n" in
    '' | *[!0-9]*) printf '4\n' ;;
    *) printf '%s\n' "$n" ;;
  esac
}
jobs="${MATRIX_JOBS:-$(detect_jobs)}"

# A diagnostic line (to stderr) so detection/discovery decisions are never
# silent. stderr keeps these off the stdout per-app table, which would otherwise
# corrupt a row mid-line (the row is printed without a trailing newline until
# its verdict is known); stdout stays the clean, parseable table + summary.
log() { printf 'board_sim matrix: %s\n' "$*" >&2; }

# The board_sim exit codes this classifier keys on, named so the tests below
# read as intent rather than digits. The full space is in sim_run.h: 0 clean,
# 1 emulation fault, 2 BKPT, 3 CPU-time guard. 1 and 2 need no name here --
# they fall through the generic non-zero FAULT rule.
readonly k_rc_ok=0        # clean run-to-budget
readonly k_rc_wall=3      # board_sim's own CPU-time guard fired
readonly k_rc_timeout=124 # the outer `timeout` fired

# Classify one run from board_sim's stdout AND its exit code.
classify_run() { # out rc -> OK|FAULT|TRUNCATED|HALT|UNKNOWN
  local out="$1" rc="$2"
  # TRUNCATED is tested FIRST, before the non-zero-exit FAULT rule below, because
  # both wall-clock statuses are non-zero and would otherwise be absorbed into
  # FAULT -- which is precisely the bug: a load-dependent stop reported as a
  # deterministic failure. A truncated run produced NO VERDICT about the app.
  #
  # With BOARD_SIM_WALL_S=0 the sim's own guard cannot fire, so in practice only
  # the outer backstop can land here; both are still recognised so that a caller
  # who re-enables the guard cannot silently reintroduce the flake.
  if [ "$rc" -eq "$k_rc_wall" ] || [ "$rc" -eq "$k_rc_timeout" ]; then
    echo TRUNCATED
    return
  fi
  # Belt-and-suspenders: board_sim prints this whenever it cut a run short on
  # CPU-time, so the text is honoured even if the status did not reach us.
  if grep -q "TRUNCATED by the wall-clock guard" <<<"$out"; then
    echo TRUNCATED
    return
  fi
  # (4)(5) A remaining non-zero exit (fault / BKPT) is FAULT and can never be OK
  # -- do not trust stdout strings alone (an emulation error outside the marker
  # set below is still caught here).
  if [ "$rc" -ne "$k_rc_ok" ]; then
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

# -- --run-one <app>: the parallel worker. ------------------------------------
# Build one app into its own per-app build dir, boot it, classify it, and write
# `<verdict><TAB><display text>` to $run_dir/<app>.result. It ALWAYS exits 0 --
# the verdict lives in the file, never in the exit status -- so one broken app
# cannot abort the pool and silently truncate the sweep.
#
# Running the sweep in parallel is only sound BECAUSE of the determinism fix
# above, and the dependency runs the other way too: board_sim's CPU-time guard
# inflates under N-way parallelism (N Unicorn JITs contend for cache and
# memory, so a fixed instruction count burns more CPU-seconds), so with the
# guard armed an app's verdict would change with -j. Bounding purely by the
# instruction-counted chunk budget makes every verdict a function of the
# instruction stream alone -- identical serially and at any -j. sil_all.sh
# reached the same conclusion for the same reason.
run_one() {
  local app="$1"
  local rf="$run_dir/$app.result"
  local src_dir elf note=""
  src_dir="$(app_src_dir "$app")"

  # Two-image TrustZone (--ns) apps need their dedicated recipe -> SPECIAL.
  if is_two_image_ns "$src_dir"; then
    printf 'SPECIAL\tSPECIAL (two-image TrustZone -- needs --ns; detected ns_image.ld)\n' >"$rf"
    return 0
  fi
  if [ -z "$src_dir" ] || ! timeout "$build_timeout" make -C "$src_dir" build \
    >"$run_dir/$app.build.log" 2>&1; then
    printf 'BUILD_FAIL\tBUILD FAIL (see %s)\n' "$run_dir/$app.build.log" >"$rf"
    return 0
  fi
  elf="$src_dir/build/$app.elf"
  if [ ! -f "$elf" ]; then
    printf 'NO_ELF\tNO ELF\n' >"$rf"
    return 0
  fi
  if is_dualcore_embedded "$elf" "$src_dir"; then
    note=" [dual-core embedded cpu1 image]"
  fi
  local out rc verdict
  # BOARD_SIM_WALL_S=0 DISABLES board_sim's CPU-time guard, leaving the run
  # bounded only by the instruction-counted chunk budget -- the deterministic
  # bound. See the DETERMINISM note in the header. The outer `timeout` is a hang
  # backstop only; if it ever fires the run is reported TRUNCATED, not judged.
  out="$(BOARD_SIM_MAX_CHUNKS="$max_chunks" BOARD_SIM_WALL_S=0 \
    timeout "$run_timeout" "$sim" "$elf" 2>&1)"
  rc=$?
  verdict="$(classify_run "$out" "$rc")"
  # Keep the emulator output for anything that did not come out clean. The
  # burn-down needs a CAUSE per app, not a total: without this the sweep knew
  # 46 examples faulted and could say nothing about why, so the debt had a
  # number and no plan. scripts/sim/matrix_triage.sh groups these.
  if [ "$verdict" != "OK" ] && [ "$verdict" != "HALT" ]; then
    printf '%s\n' "$out" >"$run_dir/$app.out"
  fi
  # One whole line to stderr as each worker finishes. The pool prints nothing
  # until aggregation, so without this a multi-minute sweep looks hung; these
  # may interleave under -j but each is atomic, and the DETERMINISTIC table is
  # printed by the parent from the result files, never from this stream.
  log "$(printf '%-30s %s' "$app" "$verdict")"
  case "$verdict" in
    OK) printf 'OK\tOK (boots + runs to budget)%s\n' "$note" >"$rf" ;;
    FAULT) printf 'FAULT\tFAULT (rc=%s -- board_sim model gap or firmware bug)%s\n' \
      "$rc" "$note" >"$rf" ;;
    TRUNCATED) printf 'TRUNCATED\tTRUNCATED (rc=%s -- wall-clock bound hit before chunk %s; NO VERDICT)%s\n' \
      "$rc" "$max_chunks" "$note" >"$rf" ;;
    HALT) printf 'HALT\tHALT (parked -- self-test done or panic)%s\n' "$note" >"$rf" ;;
    *) printf 'UNKNOWN\tUNKNOWN (ran to budget, no clean/halt marker -- REVIEW)%s\n' "$note" >"$rf" ;;
  esac
  return 0
}

# Re-exec entry point for the xargs pool below (a fully isolated process per
# app, so nothing is shared and the verdict cannot depend on the pool).
if [ "${1:-}" = "--run-one" ]; then
  run_dir="${MATRIX_RUN_DIR:?--run-one needs MATRIX_RUN_DIR}"
  sim="${MATRIX_SIM:?--run-one needs MATRIX_SIM}"
  run_one "$2"
  exit 0
fi

# -- --selftest: prove the CLASSIFIER is wired before trusting a sweep. -------
#
# This gate's failure mode is a wrong verdict, and the specific wrong verdict
# that cost real work is a wall-clock truncation reported as FAULT (#394): the
# run was cut short by host load, the app was never judged, and the matrix
# called it broken. The inverse mislabel -- truncation reported as OK -- is the
# #168 bug in its original form. Both directions are asserted here, against the
# real classify_run, so a future edit that folds TRUNCATED back into either
# neighbour fails immediately instead of at the next flake.
#
# It runs BEFORE the emulator build so it costs nothing and cannot be skipped
# by a build failure.
if [ "${1:-}" = "--selftest" ]; then
  sel_fail=0
  # want<TAB>rc<TAB>stdout -- one row per classifier path, both directions.
  while IFS=$'\t' read -r want rc text; do
    [ -z "$want" ] && continue
    got="$(classify_run "$text" "$rc")"
    if [ "$got" != "$want" ]; then
      echo "  FAIL classify_run(rc=$rc, '$text') = $got, expected $want"
      sel_fail=1
    fi
  done <<EOF
TRUNCATED	3	EXECUTED to the run budget
TRUNCATED	124	EXECUTED to the run budget
TRUNCATED	0	  => board_sim TRUNCATED by the wall-clock guard at chunk 2473 of 4000
FAULT	1	INVALID INSN @ 0x02007D72
FAULT	2	firmware executed a BKPT
FAULT	0	UNMAPPED read
OK	0	  => firmware EXECUTED to the run budget (no invalid opcode / fault).
HALT	0	firmware parked
UNKNOWN	0	some app chatter with no recognised marker
EOF
  # The two rows above that carry rc=3/124 WITH a clean-budget banner are the
  # load-bearing ones: they are exactly the shape usb_printer_vendor produced,
  # and they must never read OK (invented pass) or FAULT (invented failure).
  #
  # Must-fire direction: assert the harness itself can detect a wrong answer,
  # so a table that silently stopped being compared cannot report success.
  if [ "$(classify_run 'EXECUTED to the run budget' 3)" = "OK" ]; then
    echo "  FAIL a truncated run classified OK -- the #168 mislabel is back"
    sel_fail=1
  fi
  probe="$(classify_run 'nothing here' 99)"
  if [ "$probe" != "FAULT" ]; then
    echo "  FAIL an unrecognised non-zero status did not fall through to FAULT (got $probe)"
    sel_fail=1
  fi
  if [ "$sel_fail" -eq 0 ]; then
    echo "scripts/sim/matrix.sh --selftest: OK (truncation is its own state, both directions)"
  fi
  exit "$sel_fail"
fi

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
n_trunc=0
n_halt=0
n_build=0
n_special=0
n_unknown=0
n_skipped=0
: >"$report"

# -- Per-app matrix. ----------------------------------------------------------
run_dir="$ROOT/build/board_sim_matrix"
rm -rf "$run_dir"
mkdir -p "$run_dir"
export MATRIX_RUN_DIR="$run_dir" MATRIX_SIM="$sim"
export BUILD_TIMEOUT RUN_TIMEOUT MAX_CHUNKS
log "booting ${#apps[@]} example(s), -j $jobs ..."
printf '%s\n' "${apps[@]}" | grep -v '^$' |
  xargs -P "$jobs" -I{} bash "$ROOT/scripts/sim/matrix.sh" --run-one {} || true

# Aggregate in DISCOVERY order so the table and the report are byte-identical
# regardless of -j -- the parallelism must not reach the output either.
for app in "${apps[@]}"; do
  [ -z "$app" ] && continue
  n_total=$((n_total + 1))
  printf '  %-26s ' "$app"
  rf="$run_dir/$app.result"
  if [ ! -f "$rf" ]; then
    # A worker that produced no file crashed. That is a missing verdict, and a
    # missing verdict is never a pass -- count it where it will be seen.
    echo "UNKNOWN (worker produced no result -- crashed?)"
    printf '%-26s UNKNOWN\n' "$app" >>"$report"
    n_unknown=$((n_unknown + 1))
    continue
  fi
  verdict="$(cut -f1 "$rf")"
  cut -f2- "$rf"
  printf '%-26s %s\n' "$app" "$verdict" >>"$report"
  case "$verdict" in
    OK) n_ok=$((n_ok + 1)) ;;
    FAULT) n_fault=$((n_fault + 1)) ;;
    TRUNCATED) n_trunc=$((n_trunc + 1)) ;;
    HALT) n_halt=$((n_halt + 1)) ;;
    SPECIAL) n_special=$((n_special + 1)) ;;
    BUILD_FAIL | NO_ELF) n_build=$((n_build + 1)) ;;
    *) n_unknown=$((n_unknown + 1)) ;;
  esac
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
echo "  truncated      : $n_trunc  (wall-clock bound hit -- NO verdict, not a pass or a fail)"
echo "  halted/parked  : $n_halt"
echo "  unknown/review : $n_unknown"
echo "  booted OK      : $n_ok"
if [ "$runnable" -gt 0 ]; then
  pct=$((n_ok * 100 / runnable))
  echo "  boot coverage  : ${pct}% verified-OK of runnable (${n_ok}/${runnable})"
fi
echo "  report written : $report"

# Exit status. This script REPORTS; the enforcing gate is the ratchet that
# consumes $report (scripts/checks/matrix_ratchet.py, wired as the
# board-sim-matrix gate), because a bare "any fault fails" rule would have to
# stay unwired until the last of the 46 known faults is burned down -- which is
# how a gate ends up measuring nothing for a year.
#
# A non-zero status here still means "the sweep did not come out clean", which
# is what a human running it by hand wants. TRUNCATED counts: it is not a pass.
# SPECIAL and SKIPPED do not fail; HALT reached its budget and does not either.
[ "$n_build" -eq 0 ] && [ "$n_fault" -eq 0 ] && [ "$n_unknown" -eq 0 ] &&
  [ "$n_trunc" -eq 0 ]
