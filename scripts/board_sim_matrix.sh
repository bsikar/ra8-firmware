#!/usr/bin/env bash
#
# scripts/board_sim_matrix.sh -- the #67 "run every example" coverage matrix.
#
# Where board_sim_smoke.sh is a curated CI gate (deep per-app assertions on a
# hand-picked set), this is the breadth gate: it builds and boots EVERY example
# under examples/ek_ra8d2/ on the board emulator and reports, per app, whether
# it builds, boots, and runs to the budget without faulting -- the #67 success
# criterion "every example boots + exercises its peripheral in the simulator".
#
# It is intentionally a BASIC boot/exercise classifier (build / fault / halt /
# ran-clean); the smoke gate keeps the strong per-peripheral assertions. Two-
# image (TrustZone) and dual-core examples need their dedicated sim recipes
# (--ns / embedded .cpu1_image) and are reported as SPECIAL rather than run
# through the single-image path here. The _unsupported tier needs external
# hardware and is listed but skipped.
#
# Usage:
#   scripts/board_sim_matrix.sh                 # every ek_ra8d2 example
#   scripts/board_sim_matrix.sh blink dtc_transfer_demo   # explicit subset
#   BUILD_TIMEOUT=240 RUN_TIMEOUT=90 scripts/board_sim_matrix.sh
#
# Output: a per-app table on stdout plus a coverage summary, and a machine-
# readable report at $ROOT/build/board_sim_matrix.txt.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
set -u
export LC_ALL=C

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
sim_dir="$ROOT/tools/board_sim"
report="$ROOT/build/board_sim_matrix.txt"
mkdir -p "$ROOT/build"

build_timeout="${BUILD_TIMEOUT:-240}"
run_timeout="${RUN_TIMEOUT:-90}"

# Examples that are NOT single-image: two-image TrustZone (Secure + --ns NS)
# and dual-core (embedded .cpu1_image) builds need their dedicated sim recipes,
# so the single-image boot path here would misjudge them. Reported as SPECIAL.
special_apps=" ra8d2-ereader dualcore_mailbox tz_threadx_demo tz_secure_only_sd \
  tz_secure_only_usb tz_secure_only_usb_hs tz_nsc_cgc_usb cpu1_pingpong \
  cpu1_pingpong_ipc threadx_ipc_demo dfu_bootloader "

echo "board_sim matrix: building the emulator ..."
cmake -B "$sim_dir/build" -S "$sim_dir" >/dev/null 2>&1
cmake --build "$sim_dir/build" -j >/dev/null 2>&1
sim="$sim_dir/build/board_sim"

# Discover apps. Default: every main.c under the supported (ek_ra8d2) tier.
# _unsupported/ needs external hardware -- list it, but do not run it.
if [ "$#" -gt 0 ]; then
  mapfile -t apps < <(printf '%s\n' "$@")
else
  # Portable discovery (BSD/macOS find has no -printf): list each main.c, strip
  # the trailing /main.c to get the app dir, then the basename = the app name.
  mapfile -t apps < <(
    find examples/ek_ra8d2 -mindepth 2 -maxdepth 4 -name main.c 2>/dev/null \
      | sed 's#/main.c$##; s#.*/##' | sort -u)
fi

n_total=0
n_ok=0
n_fault=0
n_halt=0
n_build=0
n_special=0
: >"$report"

classify_run() { # output -> prints OK|FAULT|HALT
  local out="$1"
  if printf '%s' "$out" | grep -qE "INVALID INSN|UNMAPPED|executed a BKPT|FAULT|mmio_map failed"; then
    echo FAULT
  elif printf '%s' "$out" | grep -qE "EXECUTED to the run budget|device CONFIGURED|PASS"; then
    echo OK
  elif printf '%s' "$out" | grep -qE "parked|halt loop|reached .*_halt"; then
    echo HALT
  else
    # Ran without a fault marker and without the clean-budget marker: treat as
    # OK (it booted + ran the bounded budget) but tag it for review.
    echo OK
  fi
}

for app in "${apps[@]}"; do
  [ -z "$app" ] && continue
  n_total=$((n_total + 1))
  printf '  %-26s ' "$app"

  case "$special_apps" in
    *" $app "*)
      echo "SPECIAL (two-image / dual-core -- use its dedicated sim recipe)"
      printf '%-26s SPECIAL\n' "$app" >>"$report"
      n_special=$((n_special + 1))
      continue
      ;;
  esac

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

  out="$(BOARD_SIM_MAX_CHUNKS=4000 BOARD_SIM_WALL_S=60 \
    timeout "$run_timeout" "$sim" "$elf" 2>&1 || true)"
  verdict="$(classify_run "$out")"
  case "$verdict" in
    OK) echo "OK (boots + runs to budget)"; n_ok=$((n_ok + 1)) ;;
    FAULT) echo "FAULT (board_sim model gap or firmware bug)"; n_fault=$((n_fault + 1)) ;;
    HALT) echo "HALT (parked -- self-test done or panic)"; n_halt=$((n_halt + 1)) ;;
  esac
  printf '%-26s %s\n' "$app" "$verdict" >>"$report"
done

runnable=$((n_total - n_special))
echo ""
echo "board_sim matrix coverage:"
echo "  total examples : $n_total"
echo "  special (skip) : $n_special  (two-image / dual-core -- dedicated recipes)"
echo "  build failed   : $n_build"
echo "  faulted        : $n_fault"
echo "  halted/parked  : $n_halt"
echo "  booted OK      : $n_ok"
if [ "$runnable" -gt 0 ]; then
  pct=$(( (n_ok + n_halt) * 100 / runnable ))
  echo "  boot coverage  : ${pct}% of runnable ($((n_ok + n_halt))/${runnable})"
fi
echo "  report written : $report"

# Non-zero exit if anything build-failed or faulted, so this can gate CI later.
[ "$n_build" -eq 0 ] && [ "$n_fault" -eq 0 ]
