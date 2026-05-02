#!/usr/bin/env bash
#
# scripts/build_all_examples.sh -- build every examples/<app> target.
#
# Iterates each top-level examples/<app>/ directory containing a main.c,
# invokes `make <app>` from the repo root, captures pass/fail per app,
# prints a summary table at the end, and exits non-zero on any failure.
#
# Usage:
#   bash scripts/build_all_examples.sh
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$REPO_ROOT"

if [ ! -d examples ]; then
    echo "error: examples/ directory not found at $REPO_ROOT" >&2
    exit 1
fi

# Auto-discover apps: every examples/<tier>/<name>/ dir with a main.c.
# Apps live under tier directories (ek_ra8d2/, _unsupported/) but the
# build-target name is just the bare app name -- `make blink` works
# regardless of which tier directory the app lives in.
apps=()
for d in examples/*/*/; do
    d="${d%/}"
    name="$(basename "$d")"
    if [ -f "$d/main.c" ] && [ -f "$d/Makefile" ]; then
        apps+=("$name")
    fi
done

if [ "${#apps[@]}" -eq 0 ]; then
    echo "error: no buildable apps discovered under examples/" >&2
    exit 1
fi

# Sort the discovered apps alphabetically for stable output.
IFS=$'\n' apps=($(printf '%s\n' "${apps[@]}" | sort)) ; unset IFS

LOG_DIR="$REPO_ROOT/build/build_all_examples"
mkdir -p "$LOG_DIR"

echo "==> Building ${#apps[@]} apps from examples/"
echo

results=()
status_codes=()
fail_count=0

for app in "${apps[@]}"; do
    log_file="$LOG_DIR/${app}.log"
    printf "  [build] %-40s ... " "$app"
    if make "$app" >"$log_file" 2>&1; then
        results+=("PASS")
        status_codes+=(0)
        echo "PASS"
    else
        rc=$?
        results+=("FAIL")
        status_codes+=("$rc")
        fail_count=$((fail_count + 1))
        echo "FAIL (rc=$rc, log: $log_file)"
    fi
done

echo
echo "============================================================"
echo " Build summary"
echo "============================================================"
printf " %-40s  %-8s\n" "App" "Status"
printf " %-40s  %-8s\n" "----------------------------------------" "--------"
for i in "${!apps[@]}"; do
    printf " %-40s  %-8s\n" "${apps[$i]}" "${results[$i]}"
done
echo "============================================================"
total="${#apps[@]}"
pass_count=$((total - fail_count))
echo " Total: $total   Passed: $pass_count   Failed: $fail_count"
echo "============================================================"

if [ "$fail_count" -gt 0 ]; then
    exit 1
fi

exit 0
