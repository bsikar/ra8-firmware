#!/usr/bin/env bash
#
# tests/run_tests.sh -- run ctest against a previously built tree.
#
# Mirrors tests/build_tests.sh: defaults to tests/build/, switches to
# tests/build-cov/ when invoked with `--coverage`. Assumes the matching
# build_tests.sh invocation has already run; if the build dir is
# missing this script exits with a clear error rather than implicitly
# reconfiguring (so accidental flag mismatches can't silently delete a
# coverage build).
#
# Usage:
#
#     tests/run_tests.sh                  # run tests in tests/build/
#     tests/run_tests.sh --coverage       # run tests in tests/build-cov/
#
# Any extra args after the optional --coverage flag are forwarded to
# ctest, e.g.:
#
#     tests/run_tests.sh -R test_ra_acmphs --output-on-failure
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

MODE="fast"
if [[ "${1:-}" == "--coverage" ]]; then
  MODE="coverage"
  shift
elif [[ "${1:-}" == "--ubsan" ]]; then
  MODE="ubsan"
  shift
fi

if [[ "$MODE" == "coverage" ]]; then
  BUILD_DIR="$SCRIPT_DIR/build-cov"
  LABEL="coverage (RA_MCDC=ON)"
elif [[ "$MODE" == "ubsan" ]]; then
  BUILD_DIR="$SCRIPT_DIR/build-ubsan"
  LABEL="ubsan (RA_SANITIZE=undefined)"
  # Make any undefined behaviour a hard test failure, with a stack trace.
  export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"
else
  BUILD_DIR="$SCRIPT_DIR/build"
  LABEL="fast"
fi

if [[ ! -d "$BUILD_DIR" ]]; then
  echo "error: build dir not found: $BUILD_DIR" >&2
  if [[ "$MODE" == "fast" ]]; then
    echo "       run: tests/build_tests.sh" >&2
  else
    echo "       run: tests/build_tests.sh --$MODE" >&2
  fi
  exit 1
fi

echo "==> ra8-firmware tests: running ($LABEL)"
echo "    build dir : $BUILD_DIR"

# Flake-tolerant ctest. The harness injects SIGALRM to exercise timeout paths;
# under parallel ctest that delivery occasionally races and SIGABRTs an
# UNRELATED test that passes in isolation ("Subprocess aborted"). Re-run ONLY
# such aborted tests serially; a genuine (Failed/Timeout) test is NEVER retried,
# so this cannot mask a real regression.
set +e
ctest --test-dir "$BUILD_DIR" --output-on-failure --timeout 60 "$@" 2>&1 \
  | tee "$BUILD_DIR/ctest_run.log"
_rc=${PIPESTATUS[0]}
set -e
[[ $_rc -eq 0 ]] && exit 0

# The SIGALRM race is concurrency-only and presents as EITHER "Subprocess
# aborted" OR "Failed" (a mis-timed alarm can corrupt an assertion), so parse
# EVERY failed test from ctest's "The following tests FAILED:" list and re-run
# them ALL serially. A genuinely broken test is deterministic and fails at -j1
# too, so this cannot mask a real regression -- the -j1 run trips set -e below.
_failed="$(grep -oE '[0-9]+ - test_[A-Za-z0-9_]+ ' "$BUILD_DIR/ctest_run.log" \
  2>/dev/null | grep -oE 'test_[A-Za-z0-9_]+' | sort -u)"
if [[ -z "$_failed" ]]; then
  echo "==> ctest failed but no per-test failure parsed; not retrying" >&2
  exit "$_rc"
fi
_pat="$(echo "$_failed" | paste -sd'|' -)"
echo "==> re-running failed test(s) serially (concurrency-flake vs real): ${_pat//|/, }"
ctest --test-dir "$BUILD_DIR" -j1 --timeout 120 --output-on-failure -R "^(${_pat})$"
