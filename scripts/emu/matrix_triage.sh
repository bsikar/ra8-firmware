#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/emu/matrix_triage.sh -- group the ra8_emulator matrix failures by CAUSE.
#
# scripts/emu/matrix.sh answers "how many examples do not run in the
# emulator". That number alone is not a plan: unattributed faults only show the
# size of the debt and nothing about how to burn it down, and a ratchet on an
# unattributed total invites the cheapest possible fix rather than the right one.
# This groups the same sweep's failures by their emulator signature, so each
# tranche can become an issue with a scope.
#
# It reads the per-app output matrix.sh keeps for every non-clean verdict
# (build/ra8_emulator_matrix/<app>.out) plus the verdict report itself, so it
# needs no re-run -- triage is a read of the last sweep.
#
# Usage:
#   bash scripts/emu/matrix.sh            # produce the report + per-app output
#   bash scripts/emu/matrix_triage.sh     # group what it found
#   bash scripts/emu/matrix_triage.sh --selftest
#
#
# Shell options: -u and -o pipefail, deliberately NOT -e -- like matrix.sh this
# is a report that must run to completion across per-app oddities rather than
# abort on the first one. Text tests use here-strings, so pipefail has no
# SIGPIPE producer to misreport.
set -uo pipefail
export LC_ALL=C

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 1
run_dir="$ROOT/build/ra8_emulator_matrix"
report="$ROOT/build/ra8_emulator_matrix.txt"

# Cause classification, most specific FIRST -- the first match wins, so a
# signature that is a special case of a broader one must precede it.
#
# Each row is `label<TAB>extended-regex`. The label is what the burn-down is
# planned against, so it names a TRANCHE (a thing to go fix), never a symptom
# restated. An app that matches nothing lands in "unclassified", which is
# printed loudly: an unattributed fault is exactly what this tool exists to
# eliminate, so it must never be silently absorbed into a neighbouring bucket.
#
# EVERY pattern below is matched against REAL captured Zig-core ra8_emulator
# output lines (never generic telemetry lines like "bus faults: 0 raised").
triage_rules() {
  cat <<'EOF'
unknown instruction	zig core: unknown encoding at
EPSR.T / thumb state	zig core: EPSR\.T clear at
bus fault	zig core: bus fault at
invalid exception return	zig core: invalid exception return at
unaligned access	zig core: unaligned MemA access at
stack limit overrun	zig core: stack limit overrun at
BKPT / assert give-up	zig core: BKPT halted at
missing vector table	zig core: no vector table at
build failure	BUILD FAIL|NO ELF
EOF
}

# Classify one app into a cause label, from its VERDICT and its emulator output.
classify_cause() { # app [verdict] -> label on stdout
  local app="$1" verdict="${2:-}" out="" label="" pattern=""
  local f="$run_dir/$app.out"
  # TRUNCATED is a verdict, not a signature. The run is killed by the outer
  # `timeout`, so ra8_emulator never gets to print a distinguishing line and the
  # captured output looks like a healthy run that simply stops. Reading the
  # verdict is the only honest way to attribute it.
  if [ "$verdict" = "TRUNCATED" ]; then
    printf 'wall-clock truncation -- NO verdict reached\n'
    return 0
  fi
  [ -f "$f" ] && out="$(cat "$f")"
  while IFS=$'\t' read -r label pattern; do
    [ -z "$label" ] && continue
    if grep -qE "$pattern" <<<"$out"; then
      printf '%s\n' "$label"
      return 0
    fi
  done < <(triage_rules)
  printf 'unclassified\n'
}

# --selftest: prove the classifier attributes a known signature and, just as
# importantly, REFUSES to attribute an unknown one. Both directions are asserted.
if [ "${1:-}" = "--selftest" ]; then
  sel_fail=0
  probe_dir="$(mktemp -d)"
  real_run_dir="$run_dir"
  run_dir="$probe_dir"
  # Captured Zig-core ra8_emulator output shapes (every fixture carries shared
  # healthy report lines like "bus faults: 0 raised" to verify rules do not
  # greedily collapse onto non-fault telemetry).
  cat >"$probe_dir/unk_app.out" <<'REAL'
loaded 12345 bytes, vectors at 0x02000000, sp 0x220FFF00, pc 0x02000100
zig core: unknown encoding at 0x02007D72 after 1240 instructions
bus faults: 0 raised, 0 escalated to HardFault
REAL
  cat >"$probe_dir/bus_app.out" <<'REAL'
loaded 12345 bytes, vectors at 0x02000000, sp 0x220FFF00, pc 0x02000100
zig core: bus fault at 0x020204AC after 42100 instructions
bus faults: 1 raised, 0 escalated to HardFault
REAL
  cat >"$probe_dir/bkpt_app.out" <<'REAL'
loaded 12345 bytes, vectors at 0x02000000, sp 0x220FFF00, pc 0x02000100
zig core: BKPT halted at 0x02000226 after 88 instructions
bus faults: 0 raised, 0 escalated to HardFault
REAL
  cat >"$probe_dir/weird_app.out" <<'REAL'
ra8_emulator: something nobody has seen before
bus faults: 0 raised, 0 escalated to HardFault
REAL
  while IFS=$'\t' read -r app vd want; do
    got="$(classify_cause "$app" "$vd")"
    if [ "$got" != "$want" ]; then
      echo "  FAIL classify_cause($app) = '$got', expected '$want'"
      sel_fail=1
    fi
  done <<EOF
unk_app	FAULT	unknown instruction
bus_app	FAULT	bus fault
bkpt_app	FAULT	BKPT / assert give-up
weird_app	FAULT	unclassified
unk_app	TRUNCATED	wall-clock truncation -- NO verdict reached
weird_app	TRUNCATED	wall-clock truncation -- NO verdict reached
EOF
  if [ "$(classify_cause unk_app)" = "$(classify_cause bus_app)" ]; then
    echo "  FAIL two distinct causes collapsed into one bucket (a rule is too greedy)"
    sel_fail=1
  fi
  if [ "$(classify_cause weird_app)" != "unclassified" ]; then
    echo "  FAIL an unknown signature was absorbed into a named bucket"
    sel_fail=1
  fi
  rm -rf "$probe_dir"
  run_dir="$real_run_dir"
  if [ "$sel_fail" -eq 0 ]; then
    echo "matrix_triage.sh --selftest: OK (attributes known causes, refuses unknown ones)"
  fi
  exit "$sel_fail"
fi

if [ ! -f "$report" ]; then
  echo "FATAL: no matrix report at $report -- run scripts/emu/matrix.sh first." >&2
  exit 2
fi

# Tally causes over every failing app in the report.
declare -A cause_n=()
declare -A cause_apps=()
n_fail=0
while read -r app verdict; do
  [ -z "$app" ] && continue
  case "$verdict" in
    FAULT | TRUNCATED | UNKNOWN | BUILD_FAIL | NO_ELF) : ;;
    *) continue ;;
  esac
  n_fail=$((n_fail + 1))
  cause="$(classify_cause "$app" "$verdict")"
  cause_n["$cause"]=$((${cause_n["$cause"]:-0} + 1))
  cause_apps["$cause"]="${cause_apps["$cause"]:-}${app} "
done <"$report"

echo "ra8_emulator matrix -- $n_fail failing example(s) grouped by cause:"
echo ""
# Order the CAUSES by size, biggest tranche first, then print each with its own
# app list. Sorting the whole rendered block would reorder the indented app
# lines too and detach them from their heading.
while IFS=$'\t' read -r n cause; do
  [ -z "$cause" ] && continue
  printf '%4d  %s\n' "$n" "$cause"
  printf '%s\n' "${cause_apps[$cause]}" | fold -s -w 88 | sed 's/^/        /'
  echo ""
done < <(
  for cause in "${!cause_n[@]}"; do
    printf '%s\t%s\n' "${cause_n[$cause]}" "$cause"
  done | sort -rn -k1,1
)
echo "An 'unclassified' bucket is work for this tool, not a resting place:"
echo "add its signature to triage_rules() once the cause is known."
