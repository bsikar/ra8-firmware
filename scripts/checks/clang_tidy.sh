#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# ra8-firmware - clang-tidy Static Analysis Script
#
# Runs clang-tidy over every first-party C-family file in the tree, in six
# passes. Which files exist and which pass owns each one is decided in
# scripts/checks/tidy/collect.sh; where each pass gets its compile commands is
# decided in tidy/compile_db.sh and tidy/pass_args.sh; the passes themselves
# are in tidy/passes.sh.
#
# THIS FILE IS THE ONLY ENTRY POINT. The helper bodies live in tidy/*.sh and
# are sourced below, purely so no single file carries the whole driver. The
# option parsing and the run sequence stay here; each helper still has exactly
# one home, so there is still exactly one way to run this.
#
# Usage:
#   ./scripts/checks/clang_tidy.sh              # Check mode (exit non-zero on violations)
#   ./scripts/checks/clang_tidy.sh --fix        # Apply fixes in-place
#   ./scripts/checks/clang_tidy.sh --check      # Explicit check mode (same as default)
#   ./scripts/checks/clang_tidy.sh --list-files # Print the files this script lints
#   ./scripts/checks/clang_tidy.sh --selftest   # Assert scope and routing still fire
#   ./scripts/checks/clang_tidy.sh --verbose    # Verbose output
#   ./scripts/checks/clang_tidy.sh --help       # Show this help
#
# Exit codes:
#   0  no violations
#   1  clang-tidy reported violations (RC_VIOLATIONS -- the gate ratchets this)
#   2  this script could not do its job (RC_INFRA): no clang-tidy, no
#      .clang-tidy config, no helper bodies, or a file list that collapsed
#      below RA8_TIDY_FILE_FLOOR
#
# Prerequisites:
#   clang-tidy >= 16 (Ubuntu 24.04: sudo apt-get install clang-tidy)
#   cmake (to configure the test build with compile_commands.json)
#   arm-none-eabi-gcc >= 12.3 (the firmware pass needs a Cortex-M85 compiler)

# shellcheck disable=SC2034  # BUILD_DIR and the TIDY_* arrays are assigned here and READ by the helper bodies under scripts/checks/tidy/; shellcheck cannot see across the source boundary

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

# ra8_max_jobs -- the ONE canonical bounded-parallelism width (#328). Sourced
# before the tidy helpers below so invoke.sh's detect_jobs can delegate to it,
# capping the clang-tidy fan-out instead of spawning one process per core.
# shellcheck source=scripts/ci/lib/parallelism.sh
. "$SCRIPT_DIR/../ci/lib/parallelism.sh"

# use_pinned_arm_toolchain -- the firmware pass REQUIRES a cortex-m85-aware
# arm-none-eabi-gcc (12.3+) and fails loudly (RC_INFRA) without one. Set the
# pinned 13.3 up HERE so every caller -- the pre-commit hook, gate_tidy, a
# manual run -- resolves it, instead of aborting on the distro-default 12.2
# whenever the caller forgot to (#570). A no-op when the pin is absent.
# shellcheck source=scripts/ci/lib/arm_toolchain.sh
. "$SCRIPT_DIR/../ci/lib/arm_toolchain.sh"

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

# A tree this size cannot legitimately collapse to a handful of files. If
# collect_source_files returns less than this, something broke (a bad cwd, a
# renamed scan root, a failed enumeration) and "no violations found" would be
# a lie -- the previous behaviour printed "No source files found yet" and
# exited 0, which is that lie in its purest form. Measured 2026-07-28: 2127
# first-party C-family files. Below the floor this exits RC_INFRA, so the
# tidy gate sees "could not run" and refuses to ratchet, rather than reading
# an empty sweep as a clean one.
readonly RA8_TIDY_FILE_FLOOR=1700

# ---------------------------------------------------------------------------
# Helper bodies, sourced from scripts/checks/tidy/.
#
# The loop fails loudly on an empty or missing directory rather than carrying
# on with no helpers defined. A driver that silently defined nothing would
# die at the first call with "command not found" -- and in the worst case
# would collect zero files and report a clean run having linted nothing,
# which is the exact failure mode the rest of this script is written against.
# ---------------------------------------------------------------------------
_RA8_TIDY_DIR="${SCRIPT_DIR}/tidy"
_ra8_tidy_files=("${_RA8_TIDY_DIR}"/*.sh)
if [[ ! -e "${_ra8_tidy_files[0]}" ]]; then
  printf 'clang_tidy.sh: FATAL -- no helper bodies found under %s\n' "${_RA8_TIDY_DIR}" >&2
  exit 2
fi
for _ra8_tidy_file in "${_ra8_tidy_files[@]}"; do
  # shellcheck source=/dev/null
  . "${_ra8_tidy_file}"
done
unset _ra8_tidy_file _ra8_tidy_files _RA8_TIDY_DIR

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
# Run clang-tidy: collect the files, assemble the arguments every pass shares,
# route each file to its pass, then hand off to run_all_passes (tidy/passes.sh)
# which owns the pass sequence itself.
# ---------------------------------------------------------------------------
run_clang_tidy() {
  local clang_tidy="$1"
  local config_file="$FIRMWARE_DIR/.clang-tidy"

  if [[ ! -f "$config_file" ]]; then
    print_error ".clang-tidy not found at $config_file"
    exit "$RC_INFRA"
  fi

  TIDY_FIX_FLAG=()
  if [[ "$FIX_MODE" == "true" ]]; then
    TIDY_FIX_FLAG=(--fix)
  fi

  local files
  mapfile -t files < <(collect_source_files)

  if [[ ${#files[@]} -lt $RA8_TIDY_FILE_FLOOR ]]; then
    print_error "clang_tidy.sh: FATAL -- only ${#files[@]} source file(s) in scope, floor is $RA8_TIDY_FILE_FLOOR."
    print_error "  A collapsed scope reports a clean tree because it linted nothing."
    exit "$RC_INFRA"
  fi

  print_status "Running $clang_tidy on ${#files[@]} file(s)..."
  assert_scope_covered "${files[@]}"
  detect_sdk_args

  TIDY_INCLUDE_ARG=()
  local inc
  while IFS= read -r inc; do
    TIDY_INCLUDE_ARG+=(--extra-arg="$inc")
  done < <(collect_include_args)

  TIDY_DB_ARG=()
  mapfile -t TIDY_DB_ARG < <(db_pass_args)

  TIDY_LIST_DIR="$(mktemp -d)"
  route_files_into_lists "${files[@]}"

  local exit_code=0
  run_all_passes "$clang_tidy" || exit_code=1
  rm -rf "$TIDY_LIST_DIR"

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

main() {
  parse_args "$@"

  # Put the pinned Arm GNU Toolchain on PATH before anything else, so the
  # firmware pass (and the --selftest arm-includes check) resolve a
  # cortex-m85-aware compiler no matter how this script was entered. A no-op
  # when the pin is not installed; require_arm_system_includes then fails
  # loudly with remediation rather than silently linting nothing (#570).
  use_pinned_arm_toolchain

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
