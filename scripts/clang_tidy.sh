#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# ra8d2-firmware - clang-tidy Static Analysis Script
#
# Runs clang-tidy against the host-compiled test build, which includes all
# firmware source files via standard gcc/clang and produces a valid
# compile_commands.json. This approach works around the fact that the
# cross-compiler (arm-none-eabi-gcc) emits Cortex-M85-specific flags that
# LLVM cannot always parse.
#
# Usage:
#   ./scripts/clang_tidy.sh              # Check mode (exit non-zero on violations)
#   ./scripts/clang_tidy.sh --fix        # Apply fixes in-place
#   ./scripts/clang_tidy.sh --check      # Explicit check mode (same as default)
#   ./scripts/clang_tidy.sh --verbose    # Verbose output
#   ./scripts/clang_tidy.sh --help       # Show this help
#
# Prerequisites:
#   clang-tidy >= 16 (Ubuntu 24.04: sudo apt-get install clang-tidy)
#   cmake (to configure the test build with compile_commands.json)

set -euo pipefail
set +H

# ---------------------------------------------------------------------------
# Colors
# ---------------------------------------------------------------------------
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

print_status() { echo -e "${BLUE}[INFO]${NC} $1" >&2; }
print_success() { echo -e "${GREEN}[SUCCESS]${NC} $1" >&2; }
print_warning() { echo -e "${YELLOW}[WARNING]${NC} $1" >&2; }
print_error() { echo -e "${RED}[ERROR]${NC} $1" >&2; }

# ---------------------------------------------------------------------------
# Precomputed path constants
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
FIX_MODE=false
VERBOSE=false
BUILD_DIR=""

# ---------------------------------------------------------------------------
# Usage
# ---------------------------------------------------------------------------
usage() {
  echo "ra8d2-firmware - clang-tidy Static Analysis Script"
  echo ""
  echo "Usage: $0 [options]"
  echo ""
  echo "Options:"
  echo "  --check    Check for violations without modifying files (default)"
  echo "  --fix      Apply clang-tidy fixes in-place"
  echo "  --verbose  Verbose output"
  echo "  --help     Show this help message"
}

parse_args() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --check)
        FIX_MODE=false
        shift
        ;;
      --fix)
        FIX_MODE=true
        shift
        ;;
      --verbose | -v)
        VERBOSE=true
        shift
        ;;
      --help | -h)
        usage
        exit 0
        ;;
      *)
        print_error "Unknown option: $1"
        usage
        exit 1
        ;;
    esac
  done
}

# ---------------------------------------------------------------------------
# Locate clang-tidy (prefer versioned binaries, require >= 16)
# ---------------------------------------------------------------------------
find_clang_tidy() {
  local candidates=(clang-tidy-18 clang-tidy-17 clang-tidy-16 clang-tidy)
  for candidate in "${candidates[@]}"; do
    if command -v "$candidate" &>/dev/null; then
      local version
      # Use -E (POSIX-extended) so the same script works under BSD sed
      # (macOS) and GNU sed (Linux). BSD sed does not support `\+`.
      version=$("$candidate" --version 2>&1 | sed -nE 's/.*version ([0-9]+).*/\1/p' | head -1)
      if [[ -n "$version" && "$version" -ge 16 ]]; then
        echo "$candidate"
        return 0
      fi
    fi
  done
  print_error "clang-tidy >= 16 not found."
  print_error "Install with: sudo apt-get install clang-tidy"
  exit 1
}

# ---------------------------------------------------------------------------
# Configure test build to generate compile_commands.json
# ---------------------------------------------------------------------------
configure_build() {
  local tests_dir="$FIRMWARE_DIR/tests"
  BUILD_DIR="$FIRMWARE_DIR/build/tidy"

  if [[ ! -d "$tests_dir" ]]; then
    print_warning "$tests_dir does not exist yet. Falling back to a host-native"
    print_warning "configure of the top-level CMakeLists.txt."
    tests_dir="$FIRMWARE_DIR"
  fi

  print_status "Configuring test build in $BUILD_DIR ..."
  local cmake_stdout="/dev/null"
  if [[ "${VERBOSE:-}" == "true" ]]; then
    cmake_stdout="/dev/stdout"
  fi

  # Disable coverage instrumentation so the tidy build does not
  # leave .gcno files that the parallel coverage build (build/coverage)
  # would mistakenly merge under gcovr.
  cmake -B "$BUILD_DIR" -S "$tests_dir" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DCMAKE_C_FLAGS="-DUNIT_TEST -DRA_SIMULATOR_MODE" \
    -DRA_COVERAGE=OFF \
    -Wno-dev \
    >"$cmake_stdout"

  if [[ ! -f "$BUILD_DIR/compile_commands.json" ]]; then
    print_error "compile_commands.json was not generated."
    exit 1
  fi
  print_status "compile_commands.json ready."
}

# ---------------------------------------------------------------------------
# Collect firmware source files (exclude vendor paths)
# ---------------------------------------------------------------------------
collect_source_files() {
  # Lint firmware code only. Tests use assertion macros that trip
  # cognitive-complexity and function-size thresholds designed for
  # driver code; they should be linted separately with their own
  # rule set if we ever need it.
  #
  # Scope: libs/, src/, plus every examples/<tier>/<app>/ dir with main.c.
  # Excludes build/_deps/third_party/tests and the infrastructure
  # dirs (docs, cmake, scripts, fsp, STAR, .git, node_modules,
  # .github, .devcontainer, .claude).
  local roots=("$FIRMWARE_DIR/libs" "$FIRMWARE_DIR/src")
  local entry
  for entry in "$FIRMWARE_DIR"/examples/*/*/main.c; do
    [[ -f "$entry" ]] || continue
    # examples/host/* are macOS-only dev tools whose AppKit/host includes
    # are not in the host test compile_commands.json -- skip them.
    case "$entry" in */examples/host/*) continue ;; esac
    roots+=("$(dirname "$entry")")
  done
  find "${roots[@]}" \
    \( -name '*.c' -o -name '*.h' \) \
    ! -path '*/build/*' \
    ! -path '*/build-cov/*' \
    ! -path '*/_deps/*' \
    ! -path '*/third_party/*' \
    ! -path '*/tests/*' \
    ! -path '*/libs/fonts/*' \
    2>/dev/null || true
}

# ---------------------------------------------------------------------------
# Run clang-tidy
# ---------------------------------------------------------------------------
run_clang_tidy() {
  local clang_tidy="$1"
  local config_file="$FIRMWARE_DIR/.clang-tidy"

  if [[ ! -f "$config_file" ]]; then
    print_error ".clang-tidy not found at $config_file"
    exit 1
  fi

  local fix_flag=""
  if [[ "$FIX_MODE" == "true" ]]; then
    fix_flag="--fix"
  fi

  local exit_code=0
  local files
  mapfile -t files < <(collect_source_files)

  if [[ ${#files[@]} -eq 0 ]]; then
    print_warning "No source files found yet -- nothing to lint."
    exit 0
  fi

  print_status "Running $clang_tidy on ${#files[@]} file(s)..."

  # On macOS the Homebrew-installed clang/clang-tidy ships its own resource
  # directory but does NOT bundle the C standard library headers (string.h,
  # stddef.h, etc.). Those live in the Command Line Tools SDK. Without an
  # explicit -isystem the lint reports spurious "string.h file not found"
  # errors. Resolve the SDK once here so every file gets the same flag.
  local extra_sdk_arg=()
  if [[ "$(uname -s)" == "Darwin" ]]; then
    local macos_sdk
    if macos_sdk="$(xcrun --show-sdk-path 2>/dev/null)" && [[ -d "$macos_sdk/usr/include" ]]; then
      # -isystem resolves the C stdlib headers; -isysroot additionally lets
      # clang-tidy find the macOS frameworks (CoreGraphics, AppKit) that the
      # host display backend includes -- without it the Command Line Tools
      # build reports a spurious "CoreGraphics/CoreGraphics.h file not found".
      extra_sdk_arg=(--extra-arg="-isysroot" --extra-arg="$macos_sdk"
        --extra-arg="-isystem" --extra-arg="$macos_sdk/usr/include")
    fi
  fi

  # Note: no --config-file. We let clang-tidy auto-discover .clang-tidy
  # by walking up from each source file. That picks up the project-root
  # config AND per-directory overrides (e.g. examples/.clang-tidy and
  # libs/ra_nsc/src/.clang-tidy), which --config-file would suppress.
  set +e
  # The compile_commands.json captures GCC-only warning flags
  # (-Wduplicated-branches, -Wduplicated-cond, -Wlogical-op,
  # -Wformat-{overflow,truncation}=2). cmake/ra_warnings.cmake
  # gates these via $<COMPILE_LANG_AND_ID:C,GNU> generator
  # expressions so they're emitted only when CC=gcc, but the
  # generator expression resolves to literal flags in the
  # compile_commands.json, which clang-tidy then sees as
  # "unknown warning option" errors when it parses the file with
  # clang. -Wno-unknown-warning-option silences those without
  # affecting the actual GCC firmware build.
  "$clang_tidy" \
    -p="$BUILD_DIR" \
    --extra-arg="-std=c2x" \
    --extra-arg="-DUNIT_TEST" \
    --extra-arg="-DRA_SIMULATOR_MODE" \
    --extra-arg="-Wno-unknown-warning-option" \
    "${extra_sdk_arg[@]}" \
    ${fix_flag:+"$fix_flag"} \
    "${files[@]}" 2>&1
  exit_code=$?
  set -e

  if [[ $exit_code -ne 0 ]]; then
    echo ""
    print_error "clang-tidy found violations."
    if [[ "$FIX_MODE" == "true" ]]; then
      print_status "Fixes applied where possible. Review changes with git diff."
    fi
    return 1
  fi

  print_success "clang-tidy: no violations found."
  return 0
}

main() {
  parse_args "$@"

  local clang_tidy
  clang_tidy="$(find_clang_tidy)"

  if [[ "$VERBOSE" == "true" ]]; then
    print_status "Using: $($clang_tidy --version | head -1)"
  fi

  configure_build
  run_clang_tidy "$clang_tidy"
}

main "$@"
