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
# Collect first-party source files (exclude vendor paths)
# ---------------------------------------------------------------------------
collect_source_files() {
  # Scope: every first-party C translation unit and header in the repo --
  # libs/, src/, tests/, tools/, plus every examples/<tier>/<app>/ dir with a
  # main.c. CLAUDE.md ("Scope: these standards apply to EVERY first-party file
  # in the repository") makes the host tools and the host test suite subject to
  # exactly the same rules as the firmware; a file being "just a simulator" or
  # "just a test" is not a reason to relax them.
  #
  # The ONLY exemptions are vendored SOUP (libs/third_party/) and generated
  # font data (libs/fonts/), matching the CLAUDE.md exemption list. Build trees
  # and CMake-fetched deps are excluded because they are not source.
  local roots=("$FIRMWARE_DIR/libs" "$FIRMWARE_DIR/src" "$FIRMWARE_DIR/tests"
    "$FIRMWARE_DIR/tools")
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
    ! -path '*/libs/fonts/*' \
    2>/dev/null |
    while IFS= read -r f; do
      grep -qlE '#\s*include\s*[<"](tx_api|nx_api|ux_api)\.h[">]' "$f" 2>/dev/null && continue
      printf '%s\n' "$f"
    done || true
}

# ---------------------------------------------------------------------------
# Union of every -I directory in the generated compile_commands.json.
#
# clang-tidy has no compile command for a bare header, nor for a TU that the
# default test configure leaves out of the build (tests/fuzz/, tests/bench/).
# For those it *infers* one from the nearest file it does know, which routinely
# drops an include directory and turns the lint into a clang-diagnostic-error
# ("'miniz.h' file not found") instead of a real analysis. Appending the union
# of the build's own -I flags makes every first-party include resolvable no
# matter which command was inferred. It is derived from the compile database,
# so it needs no maintenance as targets gain include directories.
# ---------------------------------------------------------------------------
collect_include_args() {
  python3 - "$BUILD_DIR/compile_commands.json" <<'PY'
import json
import shlex
import sys

seen = set()
with open(sys.argv[1], encoding="utf-8") as handle:
    entries = json.load(handle)
for entry in entries:
    args = entry.get("arguments") or shlex.split(entry.get("command", ""))
    for arg in args:
        if arg.startswith("-I") and len(arg) > 2 and arg not in seen:
            seen.add(arg)
            print(arg)
PY
}

# ---------------------------------------------------------------------------
# Number of parallel clang-tidy processes.
# ---------------------------------------------------------------------------
detect_jobs() {
  if command -v nproc &>/dev/null; then
    nproc
  elif command -v sysctl &>/dev/null; then
    sysctl -n hw.ncpu 2>/dev/null || echo 4
  else
    echo 4
  fi
}

# ---------------------------------------------------------------------------
# Invoke clang-tidy over a file list, one process per file, `detect_jobs` at a
# time. Each process buffers its diagnostics to its own file so the parallel
# output never interleaves; the buffers are concatenated in list order after
# the batch drains. Returns non-zero if any file reported a violation.
#
# $1  clang-tidy binary
# $2  human-readable pass label (progress output only)
# $3  newline-separated file list
# $4+ arguments common to every invocation. A literal `--` splits clang-tidy's
#     own options (before) from the fixed compile command (after); the file
#     list is spliced in between, which is the order clang-tidy requires.
# ---------------------------------------------------------------------------
invoke_clang_tidy() {
  local clang_tidy="$1"
  local label="$2"
  local list="$3"
  shift 3

  local lead=()
  local trail=()
  local seen_sep=false
  local arg
  for arg in "$@"; do
    if [[ "$arg" == "--" && "$seen_sep" == "false" ]]; then
      seen_sep=true
      continue
    fi
    if [[ "$seen_sep" == "true" ]]; then
      trail+=("$arg")
    else
      lead+=("$arg")
    fi
  done

  local count
  count=$(wc -l <"$list" | tr -d ' ')
  [[ "$count" -eq 0 ]] && return 0

  local nj
  nj="$(detect_jobs)"
  [[ "$nj" -lt 1 ]] && nj=1
  [[ "$nj" -gt "$count" ]] && nj="$count"
  print_status "  pass '$label': $count file(s) across $nj worker(s)"

  local workdir
  workdir="$(mktemp -d)"

  # Round-robin the file list into `nj` chunks so each worker gets a mix of
  # cheap and expensive translation units. Each worker is one clang-tidy
  # process over its whole chunk, writing to its own buffer -- the buffers are
  # printed in chunk order afterwards, so parallel output never interleaves.
  awk -v n="$nj" -v d="$workdir" '{ print > (d "/chunk" (NR % n)) }' "$list"

  local idx=0
  local pids=()
  while [[ "$idx" -lt "$nj" ]]; do
    local chunk="$workdir/chunk$idx"
    if [[ -s "$chunk" ]]; then
      local chunk_files=()
      mapfile -t chunk_files <"$chunk"
      if [[ "$seen_sep" == "true" ]]; then
        "$clang_tidy" "${lead[@]}" "${chunk_files[@]}" -- "${trail[@]}" \
          >"$workdir/out$idx" 2>&1 &
      else
        "$clang_tidy" "${lead[@]}" "${chunk_files[@]}" >"$workdir/out$idx" 2>&1 &
      fi
      pids+=("$!")
    fi
    idx=$((idx + 1))
  done

  local ec=0
  local pid
  for pid in "${pids[@]}"; do
    wait "$pid" || ec=1
  done

  idx=0
  while [[ "$idx" -lt "$nj" ]]; do
    [[ -s "$workdir/out$idx" ]] && cat "$workdir/out$idx"
    idx=$((idx + 1))
  done
  rm -rf "$workdir"

  return "$ec"
}

# ---------------------------------------------------------------------------
# Arguments shared by every pass that resolves its compile command from the
# host test build's compile_commands.json.
#
# Note: no --config-file. We let clang-tidy auto-discover .clang-tidy by
# walking up from each source file. That picks up the project-root config AND
# per-directory overrides (e.g. libs/ra8_nsc/src/.clang-tidy), which
# --config-file would suppress.
#
# The compile_commands.json captures GCC-only warning flags
# (-Wduplicated-branches, -Wduplicated-cond, -Wlogical-op,
# -Wformat-{overflow,truncation}=2). cmake/ra8_warnings.cmake gates these via
# $<COMPILE_LANG_AND_ID:C,GNU> generator expressions so they're emitted only
# when CC=gcc, but the generator expression resolves to literal flags in the
# compile_commands.json, which clang-tidy then sees as "unknown warning option"
# errors when it parses the file with clang. -Wno-unknown-warning-option
# silences those without affecting the actual GCC firmware build.
# ---------------------------------------------------------------------------
db_pass_args() {
  printf '%s\n' \
    "-p=$BUILD_DIR" \
    "--extra-arg-before=-std=c2x" \
    "--extra-arg=-DUNIT_TEST" \
    "--extra-arg=-DRA8_SIMULATOR_MODE" \
    "--extra-arg=-Wno-unknown-warning-option"
}

# ---------------------------------------------------------------------------
# Arguments for the tools/ pass.
#
# The host dev tools are built by their own per-tool build files -- two CMake
# projects (board_sim, mkbookimg/mkfontimg) and three Makefiles (cache_bench,
# glyph_bench, reader_vmem) -- and none of them feed the host test compile
# database. Rather than configure five separate build trees just to obtain a
# compile command, hand clang-tidy a fixed one: the caller's include union
# (every -I the firmware build uses, so the ra8_*_regs.h headers board_sim
# consumes resolve), plus each tool's own source and inc directory, plus the
# Unicorn/Capstone headers board_sim needs, discovered through pkg-config.
#
# Nothing here is a per-file allowlist: the directories are globbed, so a new
# tool or a new tools/<tool>/inc is picked up with no edit. A tool that grows
# an include this does not cover fails LOUDLY as a clang-diagnostic-error
# rather than being silently skipped.
# ---------------------------------------------------------------------------
tools_pass_args() {
  local dir
  for dir in "$FIRMWARE_DIR"/tools/*/ "$FIRMWARE_DIR"/tools/*/inc "$FIRMWARE_DIR"/tools/*/src; do
    [[ -d "$dir" ]] && printf -- '--extra-arg=-I%s\n' "${dir%/}"
  done
  local cflag
  if command -v pkg-config &>/dev/null; then
    for cflag in $(pkg-config --cflags unicorn capstone 2>/dev/null); do
      printf -- '--extra-arg=%s\n' "$cflag"
    done
  fi
  # Homebrew installs unicorn/capstone outside the default search path and
  # ships no .pc on some formulae versions; add its include root when present.
  [[ -d /opt/homebrew/include ]] && printf -- '--extra-arg=-I/opt/homebrew/include\n'
  # CMAKE_C_EXTENSIONS is ON for every tool (board_sim) or -std=gnu23 is set
  # explicitly (the Makefile tools), so lint them as GNU C23 too.
  printf '%s\n' '--' 'cc' '-std=gnu2x'
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

  local fix_flag=()
  if [[ "$FIX_MODE" == "true" ]]; then
    fix_flag=(--fix)
  fi

  local exit_code=0
  local files
  mapfile -t files < <(collect_source_files)

  if [[ ${#files[@]} -eq 0 ]]; then
    print_warning "No source files found yet -- nothing to lint."
    exit 0
  fi

  print_status "Running $clang_tidy on ${#files[@]} file(s)..."

  # Scope self-guard (#296). tools/ and tests/ went unlinted for the whole life
  # of this script because collect_source_files quietly excluded them, and
  # nothing failed when it did -- an empty bucket looked exactly like a clean
  # one. Assert the buckets are populated so any future edit that narrows the
  # scope fails loudly instead of silently shrinking what CI checks.
  local root
  local matched
  for root in tests tools libs src; do
    matched=0
    for f in "${files[@]}"; do
      case "$f" in "$FIRMWARE_DIR/$root"/*)
        matched=1
        break
        ;;
      esac
    done
    if [[ "$matched" -eq 0 ]]; then
      print_error "Lint scope regression: no files collected under $root/."
      print_error "clang-tidy must cover every first-party root (see #296)."
      exit 1
    fi
  done

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

  local include_arg=()
  local inc
  while IFS= read -r inc; do
    include_arg+=(--extra-arg="$inc")
  done < <(collect_include_args)

  local db_arg=()
  mapfile -t db_arg < <(db_pass_args)

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
  #
  # tools/ is the third bucket: the host dev tools are built by their own
  # per-tool CMakeLists / Makefiles, none of which feed the host test compile
  # database, so they get a fixed compile command assembled from the same
  # include union plus each tool's own directory (see tools_pass_args).
  local ra8p1_list="$FIRMWARE_DIR/build/tidy-ra8p1.files"
  local tools_list="$FIRMWARE_DIR/build/tidy-tools.files"
  local main_list="$FIRMWARE_DIR/build/tidy-main.files"
  : >"$ra8p1_list"
  : >"$tools_list"
  : >"$main_list"
  local f
  for f in "${files[@]}"; do
    case "$f" in
      */tools/*) printf '%s\n' "$f" >>"$tools_list" ;;
      */examples/ra8p1_foundation/* | */ra8_npu.c | */ra8_npu.h | */ra8_npu_regs.h | \
        */ra8_npu_loader.c | */ra8_npu_loader.h | \
        */ra8_npu_quant.c | */ra8_npu_quant.h | \
        */ra8_ethosu_kernel.cc | */ra8_ethosu_kernel.h | \
        */libs/ra8_board_ra8p1/*)
        printf '%s\n' "$f" >>"$ra8p1_list"
        ;;
      *) printf '%s\n' "$f" >>"$main_list" ;;
    esac
  done

  invoke_clang_tidy "$clang_tidy" "firmware+tests" "$main_list" \
    "${fix_flag[@]}" "${db_arg[@]}" "${include_arg[@]}" "${extra_sdk_arg[@]}" ||
    exit_code=1
  # The RA8P1 pass lints TUs that are not part of the host test build, so the
  # include dirs their CMake targets add are absent from compile_commands.json.
  # Put them on the quote-include search path so those headers resolve
  # (otherwise clang-tidy reports a clang-diagnostic-error and the degraded
  # parse cascades into spurious readability findings):
  #   - libs/ra8_board_ra8p1/inc  -- "ra8_board_ra8p1.h" for the board TUs.
  #   - tools/vela/generated      -- the committed, generated .npub model header
  #                                  that examples/ra8p1_foundation/npu_vela
  #                                  includes (its CMake adds the same dir).
  invoke_clang_tidy "$clang_tidy" "ra8p1" "$ra8p1_list" \
    "${fix_flag[@]}" "${db_arg[@]}" --extra-arg="-DRA8_DEVICE_RA8P1" \
    --extra-arg="-I$FIRMWARE_DIR/libs/ra8_board_ra8p1/inc" \
    --extra-arg="-I$FIRMWARE_DIR/tools/vela/generated" \
    "${include_arg[@]}" "${extra_sdk_arg[@]}" ||
    exit_code=1
  local tools_arg=()
  mapfile -t tools_arg < <(tools_pass_args)
  invoke_clang_tidy "$clang_tidy" "tools" "$tools_list" \
    "${fix_flag[@]}" "${include_arg[@]}" "${extra_sdk_arg[@]}" "${tools_arg[@]}" ||
    exit_code=1
  rm -f "$ra8p1_list" "$tools_list" "$main_list"

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
