#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/emu/matrix.sh -- the "run every example" coverage matrix.
#
# Where smoke.sh is a curated CI gate (deep per-app assertions on a hand-picked
# set), this is the breadth gate: it builds and boots EVERY example under
# examples/ek_ra8d2/ on the board emulator and reports, per app, whether it
# builds, boots, and runs to the instruction budget without faulting.
#
# The classifier is HONEST by construction:
#
#   * The emulator build is hard-checked (`zig build` + executable binary at
#     tools/ra8_emulator/zig-out/bin/ra8_emulator) before any app is judged.
#   * Two-image TrustZone apps link a separate Non-Secure executable
#     (`<app>_ns.elf`) booted with `--ns <ns-elf>`. Dual-core apps with a
#     separate Cortex-M33 executable (`<app>_cpu1.elf`) are booted with
#     `--cpu1 <cpu1-elf>`.
#   * An app that runs to the budget with exit 0 but prints neither a clean-stop
#     marker nor a fault/halt marker is UNKNOWN (needs review), never assumed OK.
#   * ra8_emulator's exit code is factored in: a fault exit can never read OK.
#   * A run cut short by the outer `timeout` hang guard is TRUNCATED -- a distinct
#     third state, never folded into OK or FAULT.
#   * The _unsupported tier is listed as SKIPPED, never built or run, and each
#     app's own UNSUPPORTED.toml reason is printed.
#
# DETERMINISM. This matrix is a RATCHETED GATE, so runs are bounded by
# `--instructions $max_instructions`, which is purely instruction-counted and
# independent of host load or worker count (`-j`). The outer `timeout` stays
# only as a hang backstop and is sized so it is never the effective bound.
#
# Usage:
#   scripts/emu/matrix.sh                         # every ek_ra8d2 example
#   scripts/emu/matrix.sh blink dtc_transfer_demo # explicit subset
#   scripts/emu/matrix.sh --selftest              # assert the classifier
#   MATRIX_JOBS=1 scripts/emu/matrix.sh           # serial run
#   BUILD_TIMEOUT=240 RUN_TIMEOUT=600 MAX_INSTRUCTIONS=4000000 scripts/emu/matrix.sh
#
# Output: a per-app table on stdout plus a coverage summary, and a machine-
# readable report at $ROOT/build/ra8_emulator_matrix.txt.
#
# Shell options: -u and -o pipefail, deliberately NOT -e. This is a breadth
# report that must run to completion across per-app failures and tally every one
# of them. Text tests use here-strings (`grep -q ... <<<"$var"`) so pipefail
# cannot fail on SIGPIPE.
set -uo pipefail
export LC_ALL=C

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 1
# ra8_max_jobs -- the ONE canonical bounded-parallelism width.
# shellcheck source=scripts/lib/parallelism.sh
. "$ROOT/scripts/lib/parallelism.sh"

emu_dir="$ROOT/tools/ra8_emulator"
report="$ROOT/build/ra8_emulator_matrix.txt"
mkdir -p "$ROOT/build"

# Generous hang backstops so host load never truncates a normal run.
build_timeout="${BUILD_TIMEOUT:-900}"
run_timeout="${RUN_TIMEOUT:-1800}"
# Deterministic instruction budget per run.
max_instructions="${MAX_INSTRUCTIONS:-4000000}"

# Parallel workers across independent app build dirs and emulator processes.
jobs="${MATRIX_JOBS:-$(ra8_max_jobs)}"

log() { printf 'ra8_emulator matrix: %s\n' "$*" >&2; }

# Per-app device/card requirements shared with eil_all.sh.
# shellcheck source=scripts/emu/emu_fixtures.sh
. "$ROOT/scripts/emu/emu_fixtures.sh"

readonly k_rc_ok=0        # clean run-to-budget
readonly k_rc_timeout=124 # the outer `timeout` fired

# Classify one run from ra8_emulator's stdout AND its exit code.
classify_run() { # out rc -> OK|FAULT|TRUNCATED|HALT|UNKNOWN
  local out="$1" rc="$2"
  # TRUNCATED is tested FIRST, before the non-zero-exit FAULT rule below, so an
  # outer `timeout` abort is never mislabelled as an app fault or a clean pass.
  if [ "$rc" -eq "$k_rc_timeout" ]; then
    echo TRUNCATED
    return
  fi
  # Non-zero exit is FAULT and can never read OK regardless of stdout text.
  if [ "$rc" -ne "$k_rc_ok" ]; then
    echo FAULT
    return
  fi
  # Known Zig-core fatal signatures are FAULT even if rc did not reflect them.
  if grep -qE "zig core: (unknown encoding|EPSR\.T clear|bus fault|invalid exception return|BKPT halted|unaligned MemA access|stack limit overrun|no vector table)" <<<"$out"; then
    echo FAULT
    return
  fi
  # rc == 0 and no fault marker: recognised Zig-core clean completion is OK.
  if grep -qE "zig core: ran [0-9]+ instructions clean|stopped clean " <<<"$out"; then
    echo OK
    return
  fi
  # A firmware park / halt loop marker without clean-budget footer.
  if grep -qE "parked|halt loop|reached .*_halt" <<<"$out"; then
    echo HALT
    return
  fi
  echo UNKNOWN
}

# Resolve an app's source directory from its leaf name (build dirs excluded).
app_src_dir() { # name -> source dir on stdout (empty if not found)
  find examples -type d -name "$1" -not -path '*/build/*' \
    -not -path '*/build-emu/*' 2>/dev/null | head -1
}

ns_elf_args() { # secure-elf -> "--ns <ns-elf>" on stdout (empty when single-image)
  local ns="${1%.elf}_ns.elf"
  if [ -f "$ns" ]; then
    printf -- '--ns %s' "$ns"
  fi
}

cpu1_elf_args() { # primary-elf -> "--cpu1 <cpu1-elf>" on stdout (empty if single-core)
  local cpu1="${1%.elf}_cpu1.elf"
  if [ -f "$cpu1" ]; then
    printf -- '--cpu1 %s' "$cpu1"
  fi
}

is_two_image_ns() { # src-dir -> 0 (true) if it links a separate NS image
  local dir="$1"
  [ -n "$dir" ] || return 1
  [ -f "$dir/ns_image.ld" ]
}

is_dualcore_embedded() { # elf src-dir -> 0 (true) if dual-core image
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

# Build one app and resolve what ra8_emulator needs to boot it.
build_and_resolve() { # app src-dir result-file -> "elf<TAB>note<TAB>extra core args"
  local app="$1" src_dir="$2" rf="$3"
  local elf note="" core_args="" ns="" cpu1=""
  if [ -z "$src_dir" ] || ! CMAKE_BUILD_PARALLEL_LEVEL=1 \
    timeout "$build_timeout" just apps::build "$app" \
    >"$run_dir/$app.build.log" 2>&1; then
    printf 'BUILD_FAIL\tBUILD FAIL (see %s)\n' "$run_dir/$app.build.log" >"$rf"
    return 1
  fi
  elf="$src_dir/build/$app.elf"
  if [ ! -f "$elf" ]; then
    printf 'NO_ELF\tNO ELF\n' >"$rf"
    return 1
  fi
  if is_two_image_ns "$src_dir"; then
    ns="$(ns_elf_args "$elf")"
    if [ -z "$ns" ]; then
      printf 'NO_ELF\tNO NS ELF (ns_image.ld declares a Non-Secure image; %s_ns.elf was not built)\n' \
        "$app" >"$rf"
      return 1
    fi
    core_args="$ns"
    note=" [two-image TrustZone -- booted with --ns]"
  fi
  cpu1="$(cpu1_elf_args "$elf")"
  if [ -n "$cpu1" ]; then
    core_args="${core_args:+$core_args }$cpu1"
    note="${note} [dual-core -- booted with --cpu1]"
  elif is_dualcore_embedded "$elf" "$src_dir"; then
    note="${note} [dual-core embedded cpu1 image]"
  fi
  printf '%s\t%s\t%s\n' "$elf" "$note" "$core_args"
}

build_one() {
  local app="$1"
  local rf="$run_dir/$app.result"
  local src_dir resolved
  src_dir="$(app_src_dir "$app")"
  resolved="$(build_and_resolve "$app" "$src_dir" "$rf")" || return 0
  printf '%s\n' "$resolved" >"$run_dir/$app.built"
  log "$(printf '%-30s built' "$app")"
}

run_one() {
  local app="$1"
  local rf="$run_dir/$app.result"
  local elf note core_args
  [ -f "$run_dir/$app.built" ] || return 0
  IFS=$'\t' read -r elf note core_args <"$run_dir/$app.built"
  local -a extra_core=()
  [ -n "$core_args" ] && read -r -a extra_core <<<"$core_args"
  local -a dev_args=()
  read -r -a dev_args <<<"$(emu_extra_args "$app")"
  local out rc verdict
  out="$(timeout "$run_timeout" "$emu" "$elf" \
    --instructions "$max_instructions" --console \
    ${extra_core[@]+"${extra_core[@]}"} \
    ${dev_args[@]+"${dev_args[@]}"} 2>&1)"
  rc=$?
  verdict="$(classify_run "$out" "$rc")"
  if [ "$verdict" != "OK" ] && [ "$verdict" != "HALT" ]; then
    printf '%s\n' "$out" >"$run_dir/$app.out"
  fi
  log "$(printf '%-30s %s' "$app" "$verdict")"
  case "$verdict" in
    OK) printf 'OK\tOK (boots + runs to budget)%s\n' "$note" >"$rf" ;;
    FAULT) printf 'FAULT\tFAULT (rc=%s -- ra8_emulator model gap or firmware bug)%s\n' \
      "$rc" "$note" >"$rf" ;;
    TRUNCATED) printf 'TRUNCATED\tTRUNCATED (rc=%s -- outer timeout hit before %s instructions; NO VERDICT)%s\n' \
      "$rc" "$max_instructions" "$note" >"$rf" ;;
    HALT) printf 'HALT\tHALT (parked -- self-test done or panic)%s\n' "$note" >"$rf" ;;
    *) printf 'UNKNOWN\tUNKNOWN (ran to budget, no clean/halt marker -- REVIEW)%s\n' "$note" >"$rf" ;;
  esac
  return 0
}

if [ "${1:-}" = "--build-one" ] || [ "${1:-}" = "--run-one" ]; then
  run_dir="${MATRIX_RUN_DIR:?worker needs MATRIX_RUN_DIR}"
  emu="${MATRIX_EMU:?worker needs MATRIX_EMU}"
  if [ "$1" = "--build-one" ]; then
    build_one "$2"
  else
    run_one "$2"
  fi
  exit 0
fi

# -- --selftest: prove the classifier before building or running anything. ----
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
TRUNCATED	124	zig core: ran 4000000 instructions clean, pc 0x02001234
FAULT	1	zig core: bus fault at 0x020204AC after 42100 instructions
FAULT	1	zig core: BKPT halted at 0x02000226 after 88 instructions
FAULT	0	zig core: unknown encoding at 0x02007D72 after 1240 instructions
OK	0	zig core: ran 4000000 instructions clean, pc 0x02001234
OK	0	stopped clean after 2000 ms, pc 0x020317AE
HALT	0	firmware parked
UNKNOWN	0	some app chatter with no recognised marker
EOF
  if [ "$(classify_run 'zig core: ran 4000000 instructions clean, pc 0x02001234' 124)" = "OK" ]; then
    echo "  FAIL a truncated run classified OK -- the mislabel is back"
    sel_fail=1
  fi
  probe="$(classify_run 'nothing here' 99)"
  if [ "$probe" != "FAULT" ]; then
    echo "  FAIL an unrecognised non-zero status did not fall through to FAULT (got $probe)"
    sel_fail=1
  fi
  if [ "$sel_fail" -eq 0 ]; then
    echo "scripts/emu/matrix.sh --selftest: OK (truncation is its own state, both directions)"
  fi
  exit "$sel_fail"
fi

# The gate calls bare `zig` and `arm-none-eabi-nm`, and the app builds configure
# through cmake/toolchain-ra8d2.cmake. On the runner and in the devcontainer all
# of it is on PATH; on a dev box it often lives in user-space installs instead
# (the Arm toolchain under ~/opt, a zig distribution beside an older one,
# cmake/just/ninja in ~/.local/bin). Discover and export it here -- RA8_ZIG and
# RA8_ARM_TOOLCHAIN_BIN keep their documented precedence -- so a local matrix
# run needs no hand-built PATH and no sudo. After --selftest, which must stay
# tool-free.
# shellcheck source=scripts/builders/find_tools.sh
. "$ROOT/scripts/builders/find_tools.sh"
ra8_find_tools_activate

# -- Locate or build the Zig ra8_emulator binary, and HARD-CHECK it. ----------
emu_build_log="$ROOT/build/ra8_emulator_matrix_emu_build.log"
if [ -n "${RA8_EMULATOR:-}" ]; then
  emu="$RA8_EMULATOR"
  log "using prebuilt RA8_EMULATOR -> $emu"
else
  if [ ! -f "$emu_dir/build.zig" ]; then
    echo "FATAL: tools/ra8_emulator is a submodule (bsikar/ra8-emulator) and is not checked out." >&2
    echo "       Run 'git submodule update --init tools/ra8_emulator' and retry." >&2
    exit 2
  fi
  log "building the emulator with zig build ..."
  if ! (cd "$emu_dir" && zig build -Doptimize=ReleaseFast) >"$emu_build_log" 2>&1; then
    echo "FATAL: ra8_emulator zig build failed (see $emu_build_log)" >&2
    exit 2
  fi
  emu="$emu_dir/zig-out/bin/ra8_emulator"
fi
if [ ! -x "$emu" ]; then
  echo "FATAL: ra8_emulator binary missing or not executable: $emu" >&2
  exit 2
fi
log "emulator OK -> $emu"

# -- Discover apps. -----------------------------------------------------------
skipped_apps=()
if [ "$#" -gt 0 ]; then
  mapfile -t apps < <(printf '%s\n' "$@")
else
  mapfile -t app_dirs < <(
    find examples/ek_ra8d2 -mindepth 3 -path '*/src/main.c' -not -path '*/build/*' \
      -not -path '*/build-emu/*' 2>/dev/null | sed 's#/src/main.c$##' | sort -u
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
  mapfile -t skipped_apps < <(
    find examples/_unsupported -mindepth 2 -path '*/src/main.c' -not -path '*/build/*' \
      -not -path '*/build-emu/*' 2>/dev/null | sed 's#/src/main.c$##; s#.*/##' | sort -u
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
run_dir="$ROOT/build/ra8_emulator_matrix"
rm -rf "$run_dir"
mkdir -p "$run_dir"

EMU_IMPORT_READER_IMG=""
case " ${apps[*]} " in
  *" import_reader "*)
    candidate="$run_dir/import_reader.img"
    if emu_build_import_reader_card "$candidate"; then
      EMU_IMPORT_READER_IMG="$candidate"
    else
      log "WARNING import_reader fixture could not be built; its run will fail loud"
    fi
    ;;
esac
export MATRIX_RUN_DIR="$run_dir" MATRIX_EMU="$emu"
export EMU_IMPORT_READER_IMG
export BUILD_TIMEOUT RUN_TIMEOUT MAX_INSTRUCTIONS
log "building ${#apps[@]} example(s), -j $jobs ..."
printf '%s\n' "${apps[@]}" | grep -v '^$' |
  xargs -P "$jobs" -I{} bash "$ROOT/scripts/emu/matrix.sh" --build-one {} || true
log "booting $(find "$run_dir" -name '*.built' | wc -l | tr -d ' ') built example(s), -j $jobs ..."
printf '%s\n' "${apps[@]}" | grep -v '^$' |
  xargs -P "$jobs" -I{} bash "$ROOT/scripts/emu/matrix.sh" --run-one {} || true

for app in "${apps[@]}"; do
  [ -z "$app" ] && continue
  n_total=$((n_total + 1))
  printf '  %-26s ' "$app"
  rf="$run_dir/$app.result"
  if [ ! -f "$rf" ]; then
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
  marker="examples/_unsupported/$app/UNSUPPORTED.toml"
  reason=""
  if [ -f "$marker" ]; then
    reason=$(sed -n 's/^reason[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' "$marker" | head -n 1)
  fi
  [ -n "$reason" ] || reason="UNMARKED -- see scripts/checks/check_unsupported_exclusions.py"
  printf '  %-26s SKIPPED (_unsupported tier -- %s)\n' "$app" "$reason"
  printf '%-26s SKIPPED\n' "$app" >>"$report"
done

# -- Honest summary. ----------------------------------------------------------
runnable=$((n_total - n_special))
accounted=$((n_ok + n_fault + n_trunc + n_halt + n_unknown + n_build + n_special))
echo ""
echo "ra8_emulator matrix coverage:"
echo "  ek_ra8d2 examples : $n_total   (the tier this gate measures)"
echo "  + _unsupported    : $n_skipped   (separate tier, never booted -- per-app reason above)"
echo "  = report rows     : $((n_total + n_skipped))"
echo ""
echo "  build failed      : $n_build"
echo "  faulted           : $n_fault"
echo "  truncated         : $n_trunc   (outer timeout hit -- NO verdict, not a pass or a fail)"
echo "  halted/parked     : $n_halt"
echo "  unknown/review    : $n_unknown"
echo "  special (not run) : $n_special   (declared a Non-Secure image that was not built)"
echo "  booted OK         : $n_ok"
if [ "$accounted" -ne "$n_total" ]; then
  echo "  !! ACCOUNTING BUG : $accounted bucketed vs $n_total discovered -- an example is uncounted"
fi
if [ "$runnable" -gt 0 ]; then
  pct=$((n_ok * 100 / runnable))
  echo "  boot coverage     : ${pct}% verified-OK of runnable (${n_ok}/${runnable})"
fi
echo "  report written    : $report"

[ "$n_build" -eq 0 ] && [ "$n_fault" -eq 0 ] && [ "$n_unknown" -eq 0 ] &&
  [ "$n_trunc" -eq 0 ]
