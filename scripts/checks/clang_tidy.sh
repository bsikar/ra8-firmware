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
#   ./scripts/checks/clang_tidy.sh              # Check mode (exit non-zero on violations)
#   ./scripts/checks/clang_tidy.sh --fix        # Apply fixes in-place
#   ./scripts/checks/clang_tidy.sh --check      # Explicit check mode (same as default)
#   ./scripts/checks/clang_tidy.sh --verbose    # Verbose output
#   ./scripts/checks/clang_tidy.sh --help       # Show this help
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
FIRMWARE_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
FIX_MODE=false
VERBOSE=false
LIST_ONLY=false
SELFTEST=false
BUILD_DIR=""

# Exit codes. The caller MUST be able to tell "the code has findings" from
# "this script could not do its job": the tidy gate ratchets the former
# against a committed baseline, and treating a broken compile database as a
# pile of findings would let an infrastructure failure ratchet itself in.
RC_VIOLATIONS=1
RC_INFRA=2

# ---------------------------------------------------------------------------
# Usage
# ---------------------------------------------------------------------------
usage() {
  echo "ra8-firmware - clang-tidy Static Analysis Script"
  echo ""
  echo "Usage: $0 [options]"
  echo ""
  echo "Options:"
  echo "  --check       Check for violations without modifying files (default)"
  echo "  --fix         Apply clang-tidy fixes in-place"
  echo "  --list-files  Print the files this script would lint, then exit"
  echo "  --selftest    Assert the scope and pass routing still fire, then exit"
  echo "  --verbose     Verbose output"
  echo "  --help        Show this help message"
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
      # Scope introspection for check_lint_coverage.py: print exactly the file
      # list this script would lint, so the coverage gate can ask what is
      # covered rather than keep a second copy of the answer.
      --list-files)
        LIST_ONLY=true
        shift
        ;;
      --selftest)
        SELFTEST=true
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
  exit "$RC_INFRA"
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
    exit "$RC_INFRA"
  fi

  # ra8_reflow ships TWO mutually exclusive engines: the hand-rolled v1 (the
  # default) and the LiteHTML-backed v2 behind RA8_REFLOW_USE_LITEHTML. Only
  # one set of sources is compiled, so the default configure above has no
  # command for libs/ra8_reflow/v2/ra8_reflow_v2.cpp or its test -- and
  # clang-tidy then infers one, drops the litehtml include path and reports
  # "'litehtml.h' file not found" instead of analysing either file.
  #
  # Configure the other engine into its own tree and merge its entries in for
  # files the default tree does not cover. Merging (rather than switching) is
  # what keeps BOTH engines linted from one database: flipping the option would
  # simply move the blind spot onto v1.
  local alt_dir="$FIRMWARE_DIR/build/tidy-reflow-v2"
  print_status "Configuring the alternate reflow engine in $alt_dir ..."
  if cmake -B "$alt_dir" -S "$tests_dir" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DCMAKE_C_FLAGS="-DUNIT_TEST -DRA8_SIMULATOR_MODE" \
    -DRA8_COVERAGE=OFF \
    -DRA8_REFLOW_USE_LITEHTML=ON \
    -Wno-dev \
    >"$cmake_stdout" 2>&1; then
    merge_compile_db "$alt_dir/compile_commands.json" "$BUILD_DIR/compile_commands.json"
  else
    print_error "The RA8_REFLOW_USE_LITEHTML configure failed."
    print_error "Without it the v2 engine and its test cannot be parsed, so"
    print_error "this gate would report them clean having never analysed them."
    exit "$RC_INFRA"
  fi

  print_status "compile_commands.json ready."
}

# ---------------------------------------------------------------------------
# Merge compile-database $1 into $2, adding only entries for files $2 lacks.
#
# "Only what is missing" is deliberate: the primary database describes how a
# file is really built, and an alternate configure must never silently
# redefine that. It fills gaps, it does not overrule.
# ---------------------------------------------------------------------------
merge_compile_db() {
  python3 - "$1" "$2" <<'PY'
import json
import os
import sys

src_path, dst_path = sys.argv[1], sys.argv[2]
with open(dst_path, encoding="utf-8") as handle:
    dst = json.load(handle)
try:
    with open(src_path, encoding="utf-8") as handle:
        src = json.load(handle)
except OSError:
    sys.exit(0)


def key(entry):
    return os.path.realpath(os.path.join(entry["directory"], entry["file"]))


have = {key(e) for e in dst}
added = [e for e in src if key(e) not in have]
if added:
    with open(dst_path, "w", encoding="utf-8") as handle:
        json.dump(dst + added, handle, indent=1)
print(f"merged {len(added)} entry(ies) from {os.path.basename(os.path.dirname(src_path))}")
PY
}

# ---------------------------------------------------------------------------
# Collect first-party source files (exclude vendor paths)
# ---------------------------------------------------------------------------
collect_source_files() {
  # Scope: EVERY first-party C-family file in the repository -- C, C++ and
  # Objective-C, translation units and headers alike, under libs/, src/,
  # tests/, tools/, examples/, port/ and esp32/. CLAUDE.md ("Scope: these
  # standards apply to EVERY first-party file in the repository") makes the
  # host tools, the host test suite and the firmware subject to exactly the
  # same rules; a file being "just a simulator", "just a test" or "just an
  # example" is not a reason to relax them.
  #
  # The ONLY exemptions are vendored SOUP (libs/third_party/, esp32/third_party/)
  # and generated tables (libs/fonts/, tools/vela/generated/), matching the
  # CLAUDE.md exemption list. Build trees and CMake-fetched deps are excluded
  # because they are not source.
  #
  # Scope is derived from `git ls-files`, NOT from a directory glob. An earlier
  # revision globbed `examples/*/*/main.c` to find app directories, which
  # matched only the apps sitting exactly three levels deep while the tree's
  # actual layout is examples/<tier>/.../<app>/, up to five deep. That is the
  # #296 / #332 / #358 / #359 / #360 defect: a scan list that silently stopped
  # matching the tree. Deriving from git means a new directory at any depth is
  # picked up the day it lands.
  #
  # HOW A FILE GETS A COMPILE COMMAND (#369, #370)
  # ----------------------------------------------
  # clang-tidy cannot analyse a translation unit it cannot parse, and it cannot
  # parse one without the flags it is really compiled with. There are three
  # sources of those flags here, and `route_bucket` below picks between them:
  #
  #   host       the unit-test build's compile_commands.json -- describes the
  #              host-buildable C and C++ in libs/, src/, tests/ and tools/.
  #   firmware   a CROSS-COMPILE compile_commands.json built by
  #              scripts/builders/build_cross_compile_db.py, covering every
  #              cross-compiled TU in examples/, port/ and esp32/ plus the
  #              handful of libs/ and src/ TUs that include ThreadX / NetX /
  #              USBX vendor headers.
  #   fixed      a hand-assembled command, for the host dev tools whose own
  #              per-tool build files never feed any compile database.
  #
  # The firmware bucket is what #369 was about. Pointing clang-tidy at the
  # firmware against the HOST database was measured rather than assumed: it
  # surfaced 135 findings across 96 files, and EVERY ONE was a
  # clang-diagnostic-error rather than an actionable style finding. The answer
  # was never a wider glob -- it was a database that describes how those TUs
  # actually compile.
  cd "$FIRMWARE_DIR" || return 1
  git ls-files --cached --others --exclude-standard |
    grep -E '\.(c|h|cpp|cc|cxx|hpp|hh|hxx|m)$' |
    grep -E '^(libs|src|tests|tools|examples|port|esp32)/' |
    # Vendored SOUP and generated tables -- the CLAUDE.md exemption list.
    grep -Ev '^(libs/third_party/|libs/fonts/|tools/vela/generated/|esp32/third_party/)' |
    # Build trees and CMake-fetched deps are not source.
    grep -Ev '(^|/)(build|build-[^/]*|_deps)/' |
    while IFS= read -r f; do
      # Objective-C needs the macOS SDK's AppKit / CoreGraphics headers to
      # parse at all, so it can only be linted on a macOS host. Claim it only
      # where it can really be checked: `--list-files` feeds
      # check_lint_coverage.py, and claiming coverage this platform cannot
      # deliver would make the coverage matrix report a lint that never ran.
      case "$f" in
        *.m)
          [[ "$(uname -s)" == "Darwin" ]] || continue
          ;;
      esac
      printf '%s/%s\n' "$FIRMWARE_DIR" "$f"
    done || true
}

# ---------------------------------------------------------------------------
# Which pass owns a file -- the single place that decision is made.
#
# Prints one of: firmware | ra8p1 | tools | cxx | objc | host
#
# Kept as one function (rather than inlined into the loop that fills the pass
# lists) so `--selftest` can interrogate the routing directly. A router that
# quietly sent every file to one bucket would still "lint everything" and
# report success while analysing most files with the wrong flags.
# ---------------------------------------------------------------------------
route_bucket() {
  local f="$1"
  case "$f" in
    *.m) echo objc && return 0 ;;
    # C++, but cross-compiled: the first-party Ethos-U55 kernel is pulled into
    # the TFLite-micro object library by cmake/tflite_micro.cmake and exists
    # only in an RA8P1 build, so the CROSS database is the one that knows how
    # it compiles. Checked before the generic C++ rule below.
    */ra8_ethosu_kernel.cc) echo firmware && return 0 ;;
    *.cpp | *.cc | *.cxx | *.hpp | *.hh | *.hxx) echo cxx && return 0 ;;
  esac
  case "$f" in
    # Cross-compiled firmware: examples/, port/ and esp32/ in full.
    */examples/* | */port/* | */esp32/*) echo firmware && return 0 ;;
  esac
  # A libs/ or src/ TU that includes a ThreadX / NetX / USBX vendor header is
  # firmware too: the host database carries no path to those headers, so it
  # parsed as a clang-diagnostic-error and used to be skipped outright. The
  # cross database compiles it for real and therefore knows where they live.
  if grep -qlE '#\s*include\s*[<"](tx_api|nx_api|ux_api)\.h[">]' "$f" 2>/dev/null; then
    echo firmware && return 0
  fi
  case "$f" in
    */tools/*) echo tools && return 0 ;;
    */ra8_npu.c | */ra8_npu.h | */ra8_npu_regs.h | \
      */ra8_npu_loader.c | */ra8_npu_loader.h | \
      */ra8_npu_quant.c | */ra8_npu_quant.h | \
      */ra8_ethosu_kernel.h | \
      */libs/ra8_board_ra8p1/*)
      echo ra8p1 && return 0
      ;;
  esac
  echo host
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
  if [[ "$count" -eq 0 ]]; then
    # Say it out loud. An empty bucket printing nothing is indistinguishable
    # from a clean one, which is how a run over a fraction of the tree read as
    # a pass. The caller decides whether empty is legitimate (Objective-C off
    # macOS) or a bug; this just refuses to be silent about it.
    print_warning "  pass '$label': 0 file(s) -- nothing routed to this pass"
    return 0
  fi

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
# Arguments for the C++ pass.
#
# The .cpp / .cc shims (tinyxml2 EPUB + RaBook, the C++ allocator shim, the
# litehtml reflow v2 backend, the Ethos-U55 kernel wrapper) and their tests are
# host-buildable and sit in the SAME host compile database as the C -- so this
# reuses it, and only fixes up the language. `-std=c2x` from db_pass_args is a
# C standard and makes clang reject every C++ TU outright, which is why the
# C++ files needed their own pass rather than simply being added to the glob
# (#370). The C++ standard is taken from the database entry itself when the TU
# is in it; -std=c++17 is the floor the vendored tinyxml2 and litehtml need.
# ---------------------------------------------------------------------------
cxx_pass_args() {
  printf '%s\n' \
    "-p=$BUILD_DIR" \
    "--extra-arg-before=-xc++" \
    "--extra-arg-before=-std=c++17" \
    "--extra-arg=-DUNIT_TEST" \
    "--extra-arg=-DRA8_SIMULATOR_MODE" \
    "--extra-arg=-Wno-unknown-warning-option"
}

# ---------------------------------------------------------------------------
# Arguments for the Objective-C pass (macOS only -- see collect_source_files).
#
# The two .m files are the Cocoa window backends for board_sim and ra8_viewer.
# They are compiled by their tools' own build files, so like the tools/ pass
# they get a fixed compile command rather than a database entry: the tool
# include directories, and -xobjective-c so clang parses the language it is.
# The macOS SDK root is supplied separately by run_clang_tidy.
# ---------------------------------------------------------------------------
objc_pass_args() {
  local dir
  for dir in "$FIRMWARE_DIR"/tools/*/ "$FIRMWARE_DIR"/tools/*/inc "$FIRMWARE_DIR"/tools/*/src; do
    [[ -d "$dir" ]] && printf -- '--extra-arg=-I%s\n' "${dir%/}"
  done
  printf '%s\n' \
    "--extra-arg-before=-xobjective-c" \
    "--extra-arg-before=-std=gnu2x" \
    "--extra-arg=-fobjc-arc" \
    "--extra-arg=-Wno-unknown-warning-option"
}

# ---------------------------------------------------------------------------
# The cross-compile compile database, and the arguments the firmware pass
# needs on top of it.
#
# CROSS_DB_DIR is produced by scripts/builders/build_cross_compile_db.py, which
# performs the real CMake cross-configures and FAILS if any first-party
# firmware TU ends up without a compile command. That check is the reason this
# pass cannot silently shrink: a database covering less is an error there, not
# a faster run here.
#
# Two fix-ups are needed on top of the database:
#
#   * arm-none-eabi-gcc's own system include directories. The database's
#     commands name the GCC driver, but clang-tidy parses with clang, which
#     does not know where that toolchain keeps <string.h>. Without these,
#     135 of the firmware TUs failed with "'string.h' file not found" and
#     were never analysed at all. They are ASKED FOR, not hardcoded: the
#     compiler prints its own search list, so a toolchain bump moves them
#     automatically.
#   * -Wno-unknown-warning-option, for the same GCC-only warning flags the
#     host pass already has to neutralise.
# ---------------------------------------------------------------------------
CROSS_DB_DIR=""

build_cross_db() {
  CROSS_DB_DIR="$FIRMWARE_DIR/build/xtidy"
  print_status "Building the cross-compile compile database ..."
  local out
  if ! out=$(python3 "$FIRMWARE_DIR/scripts/builders/build_cross_compile_db.py" \
    --out "$CROSS_DB_DIR" --check 2>&1); then
    printf '%s\n' "$out" >&2
    print_error "The cross-compile compile database could not be built."
    print_error "Without it the firmware TUs cannot be parsed, so this gate"
    print_error "would report a clean run over code it never analysed."
    exit "$RC_INFRA"
  fi
  printf '%s\n' "$out" | tail -2 >&2
}

# Every -isystem the cross-compiler itself reports, asked of the compiler
# rather than restated here. `-E -v` prints the search list between two
# marker lines; everything indented in between is a directory.
arm_system_includes() {
  local cc="${ARM_CC:-arm-none-eabi-gcc}"
  command -v "$cc" &>/dev/null || return 0
  "$cc" -mcpu=cortex-m85 -mthumb -E -v -x c /dev/null 2>&1 |
    sed -n '/#include <\.\.\.> search starts here/,/End of search list/p' |
    grep '^ ' |
    while IFS= read -r dir; do
      dir="${dir# }"
      [[ -d "$dir" ]] && printf -- '--extra-arg=-isystem%s\n' "$dir"
    done || true
}

firmware_pass_args() {
  # readability-magic-numbers is OFF for this pass, and this is a
  # de-duplication rather than an exemption: check_magic_numbers.py owns the
  # no-magic-numbers rule over exactly these files, and it is the only one of
  # the two that models the rule correctly.
  #
  # Measured on the firmware surface: 887 magic-number findings, of which 822
  # were rows of USB descriptor byte tables -- `0x09U, 0x02U, 0x20U, ...`.
  # This project has an explicit, reasoned position on those (see the
  # `_DATA_ROW_RE` comment in check_magic_numbers.py): a row of pure numeric
  # literals is data, not logic, and the rule is meaningless for it. clang-tidy
  # has no data-row concept and cannot be taught one. The remaining 65 were
  # real, and check_magic_numbers.py now reports every one of them -- its
  # scope used to stop at `main.c` inside examples/, so a whole block of
  # hand-addressed SAU registers had been sitting unflagged. All are fixed.
  #
  # So the rule is still enforced here, by the checker that gets it right; what
  # is switched off is a second, coarser implementation of the same rule that
  # would only ever report the 822 it gets wrong.
  printf '%s\n' \
    "-p=$CROSS_DB_DIR" \
    "--checks=-readability-magic-numbers" \
    "--extra-arg=-Wno-unknown-warning-option"
  arm_system_includes
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
  local pkg
  if command -v pkg-config &>/dev/null; then
    # unicorn + capstone: tools/board_sim. libcurl: tools/media_dl's network
    # backend. Without the include dir clang-tidy cannot find the header and
    # reports clang-diagnostic-error instead of linting the file at all -- a
    # silent hole exactly like the one #296 closed, so keep this in step with
    # the runner + devcontainer package lists.
    #
    # Query one package per call on purpose: `pkg-config --cflags a b c` prints
    # NOTHING when any single package is absent, which would silently drop the
    # other two packages' include dirs and turn their TUs into parse errors.
    for pkg in unicorn capstone libcurl; do
      for cflag in $(pkg-config --cflags "$pkg" 2>/dev/null); do
        printf -- '--extra-arg=%s\n' "$cflag"
      done
    done
  fi
  # Homebrew installs unicorn/capstone outside the default search path and
  # ships no .pc on some formulae versions; add its include root when present.
  [[ -d /opt/homebrew/include ]] && printf -- '--extra-arg=-I/opt/homebrew/include\n'
  # _GNU_SOURCE mirrors tools/media_dl/CMakeLists.txt: mdl_export.c uses
  # posix_spawn_file_actions_addchdir_np() and `environ`, which glibc gates
  # behind it. Without it here the TU degrades to clang-diagnostic-error and
  # is never actually linted.
  #
  # CMAKE_C_EXTENSIONS is ON for every tool (board_sim) or -std=gnu23 is set
  # explicitly (the Makefile tools), so lint them as GNU C23 too.
  printf '%s\n' '--' 'cc' '-std=gnu2x' '-D_GNU_SOURCE'
}

# ---------------------------------------------------------------------------
# Run clang-tidy
# ---------------------------------------------------------------------------
run_clang_tidy() {
  local clang_tidy="$1"
  local config_file="$FIRMWARE_DIR/.clang-tidy"

  if [[ ! -f "$config_file" ]]; then
    print_error ".clang-tidy not found at $config_file"
    exit "$RC_INFRA"
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
  for root in tests tools libs src examples port esp32; do
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
      print_error "clang-tidy must cover every first-party root (see #296, #369)."
      exit "$RC_INFRA"
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
  # tools/ is another bucket: the host dev tools are built by their own
  # per-tool CMakeLists / Makefiles, none of which feed the host test compile
  # database, so they get a fixed compile command assembled from the same
  # include union plus each tool's own directory (see tools_pass_args).
  #
  # firmware/ is the bucket #369 added: everything cross-compiled, parsed
  # against the cross-compile database instead of the host one. cxx/ and objc/
  # are the two #370 added, for the languages the old `*.c`-only collection
  # never saw at all.
  # PER-RUN directory, never fixed paths under build/. These files used to be
  # build/tidy-<pass>.files, which two concurrent runs -- a manual one and the
  # pre-push hook, say -- silently share: the second run truncates the first's
  # lists, whole passes come back with zero files, and `invoke_clang_tidy`
  # returns early without printing anything. The run then reports success over
  # a fraction of the tree, and against the ratchet it reads as findings having
  # been burned down. Observed exactly once, costing 142 phantom fixes.
  local list_dir
  list_dir="$(mktemp -d)"
  local ra8p1_list="$list_dir/ra8p1.files"
  local tools_list="$list_dir/tools.files"
  local main_list="$list_dir/main.files"
  local firmware_list="$list_dir/firmware.files"
  local cxx_list="$list_dir/cxx.files"
  local objc_list="$list_dir/objc.files"
  : >"$ra8p1_list"
  : >"$tools_list"
  : >"$main_list"
  : >"$firmware_list"
  : >"$cxx_list"
  : >"$objc_list"
  local f
  for f in "${files[@]}"; do
    case "$(route_bucket "$f")" in
      firmware) printf '%s\n' "$f" >>"$firmware_list" ;;
      ra8p1) printf '%s\n' "$f" >>"$ra8p1_list" ;;
      tools) printf '%s\n' "$f" >>"$tools_list" ;;
      cxx) printf '%s\n' "$f" >>"$cxx_list" ;;
      objc) printf '%s\n' "$f" >>"$objc_list" ;;
      *) printf '%s\n' "$f" >>"$main_list" ;;
    esac
  done

  invoke_clang_tidy "$clang_tidy" "host C" "$main_list" \
    "${fix_flag[@]}" "${db_arg[@]}" "${include_arg[@]}" "${extra_sdk_arg[@]}" ||
    exit_code=1

  # The firmware pass. The database is built first and its own coverage check
  # must pass, so this can never analyse a subset while reporting success.
  if [[ -s "$firmware_list" ]]; then
    build_cross_db
    local firmware_arg=()
    mapfile -t firmware_arg < <(firmware_pass_args)
    invoke_clang_tidy "$clang_tidy" "firmware (cross)" "$firmware_list" \
      "${fix_flag[@]}" "${firmware_arg[@]}" "${extra_sdk_arg[@]}" ||
      exit_code=1
  fi

  local cxx_arg=()
  mapfile -t cxx_arg < <(cxx_pass_args)
  invoke_clang_tidy "$clang_tidy" "C++" "$cxx_list" \
    "${fix_flag[@]}" "${cxx_arg[@]}" "${include_arg[@]}" "${extra_sdk_arg[@]}" ||
    exit_code=1

  # Objective-C only reaches this list on macOS (collect_source_files drops it
  # elsewhere, because AppKit does not exist to parse against). Say so out loud
  # on other platforms: a bucket that is empty because the platform cannot do
  # the work must not read the same as a bucket that came back clean.
  if [[ -s "$objc_list" ]]; then
    local objc_arg=()
    mapfile -t objc_arg < <(objc_pass_args)
    invoke_clang_tidy "$clang_tidy" "Objective-C" "$objc_list" \
      "${fix_flag[@]}" "${objc_arg[@]}" "${include_arg[@]}" "${extra_sdk_arg[@]}" ||
      exit_code=1
  else
    print_warning "Objective-C not linted on $(uname -s): needs the macOS SDK (#370)."
  fi
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
  rm -rf "$list_dir"

  if [[ $exit_code -ne 0 ]]; then
    echo ""
    print_error "clang-tidy found violations."
    if [[ "$FIX_MODE" == "true" ]]; then
      print_status "Fixes applied where possible. Review changes with git diff."
    fi
    # RC_VIOLATIONS, never RC_INFRA: the tidy gate ratchets this against the
    # committed baseline, and it must be able to tell findings apart from the
    # script having failed to run at all.
    return "$RC_VIOLATIONS"
  fi

  print_success "clang-tidy: no violations found."
  return 0
}

# ---------------------------------------------------------------------------
# --selftest: prove the scope and the routing still work, in BOTH directions.
#
# Every failure mode this script has ever had was silent: a glob that stopped
# matching, a bucket that emptied, a root that was quietly dropped. None of
# them made anything go red. So assert the positive (each bucket claims the
# files it should) AND the negative (a file that must NOT land in a bucket does
# not), and fail loudly on either.
# ---------------------------------------------------------------------------
run_selftest() {
  local failures=0
  local note

  # 1. Routing: each path shape must reach its own pass. The firmware roots are
  #    the #369 regression; the C++/Objective-C rows are the #370 one.
  local -a cases=(
    "$FIRMWARE_DIR/examples/x/y/main.c:firmware"
    "$FIRMWARE_DIR/port/usbx/src/a.c:firmware"
    "$FIRMWARE_DIR/esp32/src/main.c:firmware"
    "$FIRMWARE_DIR/libs/ra8_epub/src/shim.cpp:cxx"
    "$FIRMWARE_DIR/libs/ra8_hal/src/k.cc:cxx"
    "$FIRMWARE_DIR/tools/board_sim/src/board_view.m:objc"
    "$FIRMWARE_DIR/tools/board_sim/src/x.c:tools"
    "$FIRMWARE_DIR/libs/ra8_core/src/ra8_err.c:host"
    "$FIRMWARE_DIR/libs/ra8_board_ra8p1/src/b.c:ra8p1"
  )
  local entry path want got
  for entry in "${cases[@]}"; do
    path="${entry%:*}"
    want="${entry##*:}"
    got="$(route_bucket "$path")"
    if [[ "$got" != "$want" ]]; then
      print_error "selftest: $path routed to '$got', expected '$want'"
      failures=$((failures + 1))
    fi
  done

  # 2. Scope: every first-party root must actually be claimed. This is the
  #    assertion that would have caught #296 and #369 on the day they landed.
  # NOTE ON `grep -q` AND `set -o pipefail`, which cost a debugging round here:
  # `grep -q` exits the moment it matches, so the writer upstream of it dies of
  # SIGPIPE (141) and pipefail then reports the whole pipeline as FAILED even
  # though the match succeeded. Every match test below therefore feeds grep
  # from a here-string, never through a pipe.
  local listing
  listing="$(collect_source_files)"
  local root count
  for root in libs src tests tools examples port esp32; do
    count="$(grep -c "^$FIRMWARE_DIR/$root/" <<<"$listing" || true)"
    if [[ "$count" -eq 0 ]]; then
      print_error "selftest: no files collected under $root/ -- scope regression"
      failures=$((failures + 1))
    fi
  done

  # 3. The negative direction. Vendored SOUP and generated tables must NOT be
  #    claimed: a scope that swallowed them would report coverage this project
  #    explicitly does not want, and would bury real findings under SOUP noise.
  local forbidden
  for forbidden in libs/third_party libs/fonts tools/vela/generated esp32/third_party; do
    if grep -q "^$FIRMWARE_DIR/$forbidden/" <<<"$listing"; then
      print_error "selftest: $forbidden/ is claimed but must be exempt"
      failures=$((failures + 1))
    fi
  done

  # 4. C++ and Objective-C must be present in the collection at all. Before
  #    #370 the collection matched `*.c` and `*.h` only, so both languages were
  #    invisible -- and an invisible language reports no findings forever.
  for note in cpp cc; do
    if ! grep -q "\.$note\$" <<<"$listing"; then
      print_error "selftest: no .$note files collected -- C++ is out of scope again"
      failures=$((failures + 1))
    fi
  done
  if [[ "$(uname -s)" == "Darwin" ]] && ! grep -q '\.m$' <<<"$listing"; then
    print_error "selftest: no .m files collected on macOS -- Objective-C is out of scope"
    failures=$((failures + 1))
  fi

  if [[ "$failures" -ne 0 ]]; then
    print_error "clang_tidy.sh selftest FAILED with $failures problem(s)."
    return 1
  fi
  print_success "clang_tidy.sh selftest: scope and pass routing OK"
  return 0
}

main() {
  parse_args "$@"

  if [[ "$SELFTEST" == "true" ]]; then
    run_selftest
    exit $?
  fi

  # --list-files answers "what do you cover?" and must not need clang-tidy
  # installed or a configured build tree: a missing tool would otherwise
  # shrink the reported scope to nothing, which is the exact failure this gate
  # family exists to prevent. Paths are printed repo-relative.
  if [[ "$LIST_ONLY" == "true" ]]; then
    collect_source_files | sed "s|^$FIRMWARE_DIR/||"
    exit 0
  fi

  local clang_tidy
  clang_tidy="$(find_clang_tidy)"

  if [[ "$VERBOSE" == "true" ]]; then
    print_status "Using: $($clang_tidy --version | head -1)"
  fi

  configure_build
  run_clang_tidy "$clang_tidy"
}

main "$@"
