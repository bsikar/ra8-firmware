#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# ra8-firmware - clang-tidy Static Analysis Script
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
  echo "ra8-firmware - clang-tidy Static Analysis Script"
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
    -DCMAKE_C_FLAGS="-DUNIT_TEST -DRA8_SIMULATOR_MODE" \
    -DRA8_COVERAGE=OFF \
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
  # ThreadX/NetX/USBX application TUs (e.g. src/app/ns_main.c) include vendor
  # headers (tx_api.h, ...) that the host test compile_commands.json does not
  # carry, so clang-tidy parses them with default flags and fails to find the
  # include. They are firmware NS app code, not host-linkable -- skip them the
  # same way examples/host/* is skipped above.
  find "${roots[@]}" \
    \( -name '*.c' -o -name '*.h' \) \
    ! -path '*/build/*' \
    ! -path '*/build-*/*' \
    ! -path '*/_deps/*' \
    ! -path '*/third_party/*' \
    ! -path '*/tests/*' \
    ! -path '*/libs/fonts/*' \
    2>/dev/null |
    while IFS= read -r f; do
      grep -qlE '#\s*include\s*[<"](tx_api|nx_api|ux_api)\.h[">]' "$f" 2>/dev/null && continue
      printf '%s\n' "$f"
    done || true
}

# ---------------------------------------------------------------------------
# Invoke clang-tidy once over a file list, with an optional extra device define
# (e.g. -DRA8_DEVICE_RA8P1). Relies on run_clang_tidy's `fix_flag` and
# `extra_sdk_arg` locals being in scope (bash dynamic scoping). Returns
# clang-tidy's own exit code.
# ---------------------------------------------------------------------------
invoke_clang_tidy() {
  local clang_tidy="$1"
  local device_def="$2"
  shift 2

  local device_arg=()
  if [[ -n "$device_def" ]]; then
    # The RA8P1 pass also lints libs/ra8_board_ra8p1/* -- board TUs that are
    # not part of the host test build, so their own inc/ dir is absent from the
    # compile_commands.json. Put it on the quote-include search path so
    # "ra8_board_ra8p1.h" resolves (otherwise clang-tidy reports a
    # clang-diagnostic-error and the degraded parse cascades into spurious
    # readability findings). Harmless for the npu / example TUs in the same pass.
    device_arg=(--extra-arg="$device_def"
      --extra-arg="-I$FIRMWARE_DIR/libs/ra8_board_ra8p1/inc")
  fi

  # Note: no --config-file. We let clang-tidy auto-discover .clang-tidy
  # by walking up from each source file. That picks up the project-root
  # config AND per-directory overrides (e.g. examples/.clang-tidy and
  # libs/ra8_nsc/src/.clang-tidy), which --config-file would suppress.
  #
  # The compile_commands.json captures GCC-only warning flags
  # (-Wduplicated-branches, -Wduplicated-cond, -Wlogical-op,
  # -Wformat-{overflow,truncation}=2). cmake/ra8_warnings.cmake gates these via
  # $<COMPILE_LANG_AND_ID:C,GNU> generator expressions so they're emitted only
  # when CC=gcc, but the generator expression resolves to literal flags in the
  # compile_commands.json, which clang-tidy then sees as "unknown warning
  # option" errors when it parses the file with clang.
  # -Wno-unknown-warning-option silences those without affecting the actual GCC
  # firmware build.
  local ec=0
  set +e
  "$clang_tidy" \
    -p="$BUILD_DIR" \
    --extra-arg-before="-std=c2x" \
    --extra-arg="-DUNIT_TEST" \
    --extra-arg="-DRA8_SIMULATOR_MODE" \
    --extra-arg="-Wno-unknown-warning-option" \
    "${device_arg[@]}" \
    "${extra_sdk_arg[@]}" \
    ${fix_flag:+"$fix_flag"} \
    "$@" 2>&1
  ec=$?
  set -e
  return $ec
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

  # The RA8P1 build-foundation apps (examples/ra8p1_foundation/*), the
  # RA8P1-only Ethos-U55 NPU driver TUs (libs/ra8_hal/{src/ra8_npu.c,
  # inc/ra8_npu.h,inc/ra8_npu_regs.h}, plus the #227/#228 additions
  # ra8_npu_loader.*, ra8_npu_quant.*, ra8_ethosu_kernel.{h,cc}), and the
  # RA8P1 board layer (libs/ra8_board_ra8p1/*) carry -- directly or through
  # ra8_ethosu_shim.h / ra8_device.h -- an `#error` guard that fires unless
  # RA8_DEVICE_RA8P1 is defined; they are meant to be built ONLY with
  # cmake/toolchain-ra8p1.cmake. clang-tidy compiles every example / lints every
  # header in the DEFAULT (RA8D2) device context, so those files must be linted
  # in a second pass that adds the RA8P1 device define; otherwise they abort at
  # the guard with a clang-diagnostic-error instead of being analysed (the
  # standalone `ra8_npu` headers hit the guard; ra8_npu.c is an empty TU without the
  # define). The define is build-config only -- it does not change what the
  # readability / bugprone checks report on their bodies (the concise-preprocessor
  # nit still fires).
  local ra8p1_files=()
  local main_files=()
  local f
  for f in "${files[@]}"; do
    case "$f" in
      */examples/ra8p1_foundation/* | */ra8_npu.c | */ra8_npu.h | */ra8_npu_regs.h \
        | */ra8_npu_loader.c | */ra8_npu_loader.h \
        | */ra8_npu_quant.c | */ra8_npu_quant.h \
        | */ra8_ethosu_kernel.cc | */ra8_ethosu_kernel.h \
        | */libs/ra8_board_ra8p1/*)
        ra8p1_files+=("$f")
        ;;
      *) main_files+=("$f") ;;
    esac
  done

  if [[ ${#main_files[@]} -gt 0 ]]; then
    invoke_clang_tidy "$clang_tidy" "" "${main_files[@]}" || exit_code=1
  fi
  if [[ ${#ra8p1_files[@]} -gt 0 ]]; then
    invoke_clang_tidy "$clang_tidy" "-DRA8_DEVICE_RA8P1" "${ra8p1_files[@]}" || exit_code=1
  fi

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
