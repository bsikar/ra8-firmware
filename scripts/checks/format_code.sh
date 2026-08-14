#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# ra8-firmware Code Formatting Script for Mac/Linux
# Usage: ./scripts/checks/format_code.sh [options]

set -e # Exit on any error
set +H # Disable history expansion (fixes ! in if statements)

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Default values
CHECK_ONLY=false
VERBOSE=false
LIST_ONLY=false

# Pin the clang-format binary via env var so CI can lock to a specific
# major version (Ubuntu 24.04 ships v18 by default; Homebrew on macOS
# ships v22). Both produce slightly different formatting on edge cases
# so the check side and the auto-format side must agree on which
# version they invoke. Default to plain `clang-format` for backward
# compatibility on developer machines.
CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"

# Print usage information
usage() {
  echo "ra8-firmware Code Formatting Script"
  echo ""
  echo "Usage: $0 [options]"
  echo ""
  echo "Options:"
  echo "  -c, --check    Check formatting without making changes"
  echo "  -v, --verbose  Enable verbose output"
  echo "  -h, --help     Show this help message"
  echo ""
  echo "Examples:"
  echo "  $0             # Format all source files"
  echo "  $0 --check     # Check formatting without changes"
  echo "  $0 -v          # Format with verbose output"
}

# Print colored output
print_status() { echo -e "${BLUE}[INFO]${NC} $1" >&2; }
print_success() { echo -e "${GREEN}[SUCCESS]${NC} $1" >&2; }
print_warning() { echo -e "${YELLOW}[WARNING]${NC} $1" >&2; }
print_error() { echo -e "${RED}[ERROR]${NC} $1" >&2; }

# Check if clang-format is installed
check_clang_format() {
  if ! command -v "$CLANG_FORMAT" &>/dev/null; then
    print_error "$CLANG_FORMAT not found!"
    echo ""
    echo "Please install clang-format:"
    echo "  macOS: brew install clang-format"
    echo "  Ubuntu/Debian: sudo apt-get install clang-format"
    echo "  Fedora: sudo dnf install clang"
    exit 1
  fi

  local version
  version=$("$CLANG_FORMAT" --version | head -n1)
  if [ "$VERBOSE" = true ]; then
    print_status "Found $version"
  fi
}

# Check if .clang-format file exists
check_clang_format_config() {
  if [ ! -f ".clang-format" ]; then
    print_error ".clang-format configuration file not found!"
    echo "Please ensure you're running this script from the project root directory."
    exit 1
  fi

  if [ "$VERBOSE" = true ]; then
    print_status "Found .clang-format configuration"
  fi
}

# Parse command line arguments
parse_args() {
  while [[ $# -gt 0 ]]; do
    case $1 in
      -c | --check)
        CHECK_ONLY=true
        shift
        ;;
      -v | --verbose)
        VERBOSE=true
        shift
        ;;
      # Scope introspection for check_lint_coverage.py: print exactly the file
      # list this script would format, so the coverage gate can ask what is
      # covered rather than keep a second copy of the answer.
      --list-files)
        LIST_ONLY=true
        shift
        ;;
      -h | --help)
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

# Find all source files.
#
# Scope is derived from `git ls-files`, NOT from a directory list. The previous
# revision walked a hand-maintained set of roots (libs, src, tests,
# examples/*/*, tools/*) and so formatted nothing under port/ -- 43
# first-party C files that no formatter had ever touched, carrying 477
# clang-format violations. That is the #296 / #332 / #358 / #359 / #360 defect
# class: a scan list narrower than the tree, reporting "All files are properly
# formatted".
#
# Deriving from git means a new top-level directory is covered the day it is
# added, with no list to remember. Vendored and generated trees are excluded by
# path prefix below, matching the CLAUDE.md exemption list.
find_source_files() {
  git ls-files --cached --others --exclude-standard |
    grep -E '\.(c|h|cpp|hpp|cc|cxx|hh|hxx|m)$' |
    grep -Ev '^(libs/third_party/|libs/ra8_fonts/|tools/vela/generated/)' |
    grep -Ev '^libs/ra8_c6link/(inc|src)/ra8_media_download\.pb-c\.(c|h)$' |
    grep -Ev '(^|/)(build|build-[^/]*|_deps)/' |
    sort
}

# Check formatting of files
check_formatting() {
  local files=("$@")
  local issues_found=false

  print_status "Checking code formatting..."

  for file in "${files[@]}"; do
    if [ "$VERBOSE" = true ]; then
      echo "  Checking: $file" >&2
    fi

    if ! "$CLANG_FORMAT" --dry-run --Werror "$file" >/dev/null 2>&1; then
      if [ "$issues_found" = false ]; then
        echo "" >&2
        print_warning "Formatting issues found in:"
        issues_found=true
      fi
      echo "  $file" >&2
    fi
  done

  # Comment-format gate. clang-format owns the START column of trailing
  # comments (AlignTrailingComments) but never touches a block comment's
  # interior, so it cannot enforce "one space after /*", "one space before
  # */", or align the closing */ of a run. check_comment_format.py owns those.
  # It runs AFTER the clang-format check above so it sees start-aligned
  # comments. See scripts/checks/check_comment_format.py.
  local comment_ok=true
  if ! command -v python3 &>/dev/null; then
    # A gate whose dependency is absent must FAIL, not wave the tree through.
    # This arm used to warn and continue, so a runner without python3 reported
    # "properly formatted" having checked no comment in the tree at all.
    print_error "python3 not found -- the comment-format check cannot run."
    return 1
  fi
  if ! python3 scripts/checks/check_comment_format.py "${files[@]}"; then
    comment_ok=false
  fi

  if [ "$issues_found" = true ]; then
    echo "" >&2
    print_error "Code formatting check failed!"
    echo "Run 'bash scripts/checks/format_code.sh' to fix formatting issues." >&2
  fi
  if [ "$issues_found" = true ] || [ "$comment_ok" = false ]; then
    return 1
  fi
  print_success "All files are properly formatted!"
  return 0
}

# Run clang-format in place over the file list; echo the number of files changed.
run_clang_round() {
  local files=("$@")
  local changed=0 file temp_file
  for file in "${files[@]}"; do
    temp_file=$(mktemp)
    "$CLANG_FORMAT" "$file" >"$temp_file" 2>&1 || {
      echo "ERROR: clang-format failed on $file" >&2
      rm "$temp_file"
      continue
    }
    if ! cmp -s "$file" "$temp_file" 2>/dev/null; then
      cp "$temp_file" "$file"
      ((changed++)) || true
    fi
    rm "$temp_file"
  done
  echo "$changed"
}

# Format files. clang-format owns the comment START column; the comment pass
# (check_comment_format.py) owns the interior + the */ end column. The two are
# run to a joint fixed point: a comment that the pass tightens can free column
# budget that lets clang-format re-align the start on the next round, so we
# repeat clang-format + the pass until a whole round changes nothing (this
# converges in 2 rounds in practice; the cap is a safety bound).
format_files() {
  local files=("$@")
  local max_rounds=5 round clang_changed comment_changed comment_out total=0

  print_status "Formatting source files..."

  if ! command -v python3 &>/dev/null; then
    print_warning "python3 not found -- comment-format pass SKIPPED."
  fi

  for ((round = 1; round <= max_rounds; round++)); do
    clang_changed=$(run_clang_round "${files[@]}")

    comment_changed=0
    if command -v python3 &>/dev/null; then
      comment_out=$(python3 scripts/checks/check_comment_format.py --fix "${files[@]}" 2>&1) ||
        print_warning "comment-format pass reported a problem."
      echo "$comment_out" >&2
      comment_changed=$(echo "$comment_out" | sed -n 's/.*reformatted \([0-9][0-9]*\) file.*/\1/p')
      comment_changed=${comment_changed:-0}
    fi

    total=$((total + clang_changed + comment_changed))
    if [ "$clang_changed" -eq 0 ] && [ "$comment_changed" -eq 0 ]; then
      break
    fi
  done

  if [ "$total" -gt 0 ]; then
    print_success "Formatted (converged in $round round(s) of clang-format + comment pass)."
  else
    print_success "All files were already properly formatted!"
  fi
}

# Main execution
main() {
  # Change to project root directory
  cd "$(dirname "$0")/../.."

  parse_args "$@"

  # --list-files answers "what do you cover?" and must not need a formatter
  # installed: a missing clang-format would otherwise shrink the reported
  # scope to nothing, which is the failure mode this whole gate family exists
  # to prevent.
  if [ "$LIST_ONLY" = true ]; then
    find_source_files
    exit 0
  fi

  check_clang_format
  check_clang_format_config

  IFS=$'\n' read -d '' -r -a source_files < <(find_source_files && printf '\0')

  if [ ${#source_files[@]} -eq 0 ]; then
    print_error "No source files found -- refusing to report success."
    exit 1
  fi

  if [ "$VERBOSE" = true ]; then
    print_status "Found ${#source_files[@]} source file(s)"
  fi

  if [ "$CHECK_ONLY" = true ]; then
    check_formatting "${source_files[@]}"
  else
    format_files "${source_files[@]}"
  fi
}

main "$@"
