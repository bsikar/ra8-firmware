#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
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
#     tests/run_tests.sh -R test_ra8_acmphs --output-on-failure
#

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
  LABEL="coverage (RA8_MCDC=ON)"
elif [[ "$MODE" == "ubsan" ]]; then
  BUILD_DIR="$SCRIPT_DIR/build-ubsan"
  LABEL="ubsan (RA8_SANITIZE=undefined)"
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

ctest --test-dir "$BUILD_DIR" --output-on-failure --timeout 60 "$@"
