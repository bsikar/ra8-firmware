#!/usr/bin/env bash
#
# scripts/sim/matrix_triage.sh -- group the board_sim matrix failures by CAUSE.
#
# scripts/sim/matrix.sh answers "how many examples do not run in the
# simulator". That number alone is not a plan: 46 faults with no attribution
# tells you the size of the debt and nothing about how to burn it down, and a
# ratchet on an unattributed total invites the cheapest possible fix rather
# than the right one. This groups the same sweep's failures by their emulator
# signature, so each tranche can become an issue with a scope.
#
# It reads the per-app output matrix.sh keeps for every non-clean verdict
# (build/board_sim_matrix/<app>.out) plus the verdict report itself, so it
# needs no re-run -- triage is a read of the last sweep.
#
# Usage:
#   bash scripts/sim/matrix.sh            # produce the report + per-app output
#   bash scripts/sim/matrix_triage.sh     # group what it found
#   bash scripts/sim/matrix_triage.sh --selftest
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
# Shell options: -u and -o pipefail, deliberately NOT -e -- like matrix.sh this
# is a report that must run to completion across per-app oddities rather than
# abort on the first one. Text tests use here-strings, so pipefail has no
# SIGPIPE producer to misreport.
set -uo pipefail
export LC_ALL=C

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 1
run_dir="$ROOT/build/board_sim_matrix"
report="$ROOT/build/board_sim_matrix.txt"

# Cause classification, most specific FIRST -- the first match wins, so a
# signature that is a special case of a broader one must precede it.
#
# Each row is `label<TAB>extended-regex`. The label is what the burn-down is
# planned against, so it names a TRANCHE (a thing to go fix), never a symptom
# restated. An app that matches nothing lands in "unclassified", which is
# printed loudly: an unattributed fault is exactly what this tool exists to
# eliminate, so it must never be silently absorbed into a neighbouring bucket.
triage_rules() {
  cat <<'EOF'
MVE/Helium store (#396)	ED80 7F31|coproc.*0b1111|coprocessor 1111
MVE/Helium other	MVE|Helium|vstr|vldr
invalid instruction	INVALID INSN
unmapped access	UNMAPPED|mmio_map failed
BKPT / assert give-up	executed a BKPT
build failure	BUILD FAIL|NO ELF
EOF
}

# Classify one app's emulator output into a cause label.
classify_cause() { # app -> label on stdout
  local app="$1" out="" label="" pattern=""
  local f="$run_dir/$app.out"
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
# importantly, REFUSES to attribute an unknown one. A triage tool that quietly
# labelled everything "unclassified" would look like it ran; one that matched
# everything to the first bucket would be worse. Both directions are asserted.
if [ "${1:-}" = "--selftest" ]; then
  sel_fail=0
  probe_dir="$(mktemp -d)"
  real_run_dir="$run_dir"
  run_dir="$probe_dir"
  printf 'board_sim: INVALID INSN ED80 7F31 @ 0x0200\n' >"$probe_dir/mve_app.out"
  printf 'board_sim: UNMAPPED read @ 0x40000000\n' >"$probe_dir/unmapped_app.out"
  printf 'board_sim: something nobody has seen before\n' >"$probe_dir/weird_app.out"
  while IFS=$'\t' read -r app want; do
    got="$(classify_cause "$app")"
    if [ "$got" != "$want" ]; then
      echo "  FAIL classify_cause($app) = '$got', expected '$want'"
      sel_fail=1
    fi
  done <<EOF
mve_app	MVE/Helium store (#396)
unmapped_app	unmapped access
weird_app	unclassified
EOF
  # Must-fire direction: an unknown signature must NOT be absorbed into a real
  # bucket. If this ever starts matching, the rules have grown too greedy and
  # the triage stops being evidence.
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
  echo "FATAL: no matrix report at $report -- run scripts/sim/matrix.sh first." >&2
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
  cause="$(classify_cause "$app")"
  cause_n["$cause"]=$((${cause_n["$cause"]:-0} + 1))
  cause_apps["$cause"]="${cause_apps["$cause"]:-}${app} "
done <"$report"

echo "board_sim matrix -- $n_fail failing example(s) grouped by cause:"
echo ""
for cause in "${!cause_n[@]}"; do
  printf '%4d  %s\n' "${cause_n[$cause]}" "$cause"
  printf '        %s\n' "${cause_apps[$cause]}" | fold -s -w 96 | sed '2,$s/^/        /'
done | sort -rn
echo ""
echo "An 'unclassified' bucket is work for this tool, not a resting place:"
echo "add its signature to triage_rules() once the cause is known."
