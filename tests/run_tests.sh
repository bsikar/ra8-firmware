#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# tests/run_tests.sh -- run ctest against a previously built tree.
#
# Mirrors tests/build_tests.sh: defaults to tests/build-<os>/, switches to
# tests/build-cov-<os>/ when invoked with `--coverage`, and uses the dedicated
# UBSan tree for `--ubsan`. Assumes the matching build_tests.sh invocation has
# already run; if the build dir is
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

OS_SUFFIX=""
if [ "$(uname -s)" = "Linux" ]; then
  OS_SUFFIX="-linux"
elif [ "$(uname -s)" = "Darwin" ]; then
  OS_SUFFIX="-darwin"
fi

if [[ "$MODE" == "coverage" ]]; then
  BUILD_DIR="$SCRIPT_DIR/build-cov${OS_SUFFIX}"
  LABEL="coverage (RA8_MCDC=ON)"
elif [[ "$MODE" == "ubsan" ]]; then
  BUILD_DIR="$SCRIPT_DIR/build-ubsan${OS_SUFFIX}"
  LABEL="ubsan (RA8_SANITIZE=undefined)"
  # Make any undefined behaviour a hard test failure, with a stack trace.
  export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"
else
  BUILD_DIR="$SCRIPT_DIR/build${OS_SUFFIX}"
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

# CTest exits successfully when a configuration accidentally registers zero
# tests.  It also stayed green when the application-test glob lost 36 suites
# during the products-tier move.  Freeze the recovered population as a
# shrink-only floor: adding tests needs no maintenance, while an intentional
# removal must explain and update this number in the same change.
# Five downloader test sources are helper translation units compiled into their
# owning canonical executables; they are intentionally not standalone CTest
# cases. The resulting clean macOS/Linux registration floor is 691; two
# Linux-specific Alphabet Soup cases stay registered but disabled on macOS.
MIN_TEST_COUNT=691
TEST_COUNT="$(ctest --test-dir "$BUILD_DIR" --show-only=json-v1 | python3 -c \
  'import json, sys; print(len(json.load(sys.stdin).get("tests", [])))')"
if [[ ! "$TEST_COUNT" =~ ^[0-9]+$ ]] || ((TEST_COUNT < MIN_TEST_COUNT)); then
  echo "error: only $TEST_COUNT host test(s) registered; floor is $MIN_TEST_COUNT" >&2
  echo "       a successful CTest run over a shrunken suite is not a valid result" >&2
  exit 1
fi
echo "    tests     : $TEST_COUNT (floor: $MIN_TEST_COUNT)"

ctest --test-dir "$BUILD_DIR" --output-on-failure --timeout 60 "$@"
