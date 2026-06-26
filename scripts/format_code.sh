#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# ra8d2-firmware Code Formatting Script for Mac/Linux
# Usage: ./scripts/format_code.sh [options]

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
EXTENSIONS=("*.c" "*.h" "*.cpp" "*.hpp" "*.cc" "*.cxx" "*.hh" "*.hxx" "*.m")

# Pin the clang-format binary via env var so CI can lock to a specific
# major version (Ubuntu 24.04 ships v18 by default; Homebrew on macOS
# ships v22). Both produce slightly different formatting on edge cases
# so the check side and the auto-format side must agree on which
# version they invoke. Default to plain `clang-format` for backward
# compatibility on developer machines.
CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"
# Scan every top-level dir except infrastructure / vendor / build trees.
# build/docs/cmake/scripts/fsp/.git/etc. are excluded via -not -path in find_source_files.
DIRECTORIES=()

# Print usage information
usage() {
  echo "ra8d2-firmware Code Formatting Script"
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

# Auto-discover scan dirs: libs/, src/, tests/, plus every examples/<app>/ and
# tools/<tool>/ directory. The recursive find inside each dir picks up nested
# sources (e.g. tools/board_sim/{inc,src}); the build/ dir is excluded below.
discover_source_dirs() {
  local out=()
  for entry in libs src tests; do
    [ -d "$entry" ] && out+=("$entry")
  done
  if [ -d examples ]; then
    for app in examples/*/*; do
      [ -d "$app" ] && out+=("$app")
    done
  fi
  if [ -d tools ]; then
    for tool in tools/*; do
      [ -d "$tool" ] && out+=("$tool")
    done
  fi
  printf '%s\n' "${out[@]}"
}

# Find all source files
find_source_files() {
  local files=()

  if [ "${#DIRECTORIES[@]}" -eq 0 ]; then
    IFS=$'\n' read -d '' -r -a DIRECTORIES < <(discover_source_dirs && printf '\0')
  fi

  for dir in "${DIRECTORIES[@]}"; do
    if [ ! -d "$dir" ]; then
      if [ "$VERBOSE" = true ]; then
        print_warning "Directory '$dir' not found, skipping..."
      fi
      continue
    fi

    for ext in "${EXTENSIONS[@]}"; do
      while IFS= read -r -d '' file; do
        files+=("$file")
      done < <(find "$dir" -name "$ext" -type f \
        -not -path "*/build/*" \
        -not -path "*/build-cov/*" \
        -not -path "*/_deps/*" \
        -not -path "*/third_party/*" \
        -print0 2>/dev/null)
    done
  done

  printf '%s\n' "${files[@]}"
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
  # comments. See scripts/utils/check_comment_format.py.
  local comment_ok=true
  if command -v python3 &>/dev/null; then
    if ! python3 scripts/utils/check_comment_format.py "${files[@]}"; then
      comment_ok=false
    fi
  else
    print_warning "python3 not found -- comment-format check SKIPPED."
  fi

  if [ "$issues_found" = true ]; then
    echo "" >&2
    print_error "Code formatting check failed!"
    echo "Run 'bash scripts/format_code.sh' to fix formatting issues." >&2
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
      comment_out=$(python3 scripts/utils/check_comment_format.py --fix "${files[@]}" 2>&1) ||
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
  cd "$(dirname "$0")/.."

  parse_args "$@"

  check_clang_format
  check_clang_format_config

  # Discover top-level source dirs first so the status print can name them.
  IFS=$'\n' read -d '' -r -a DIRECTORIES < <(discover_source_dirs && printf '\0')
  print_status "Searching for source files in: ${DIRECTORIES[*]}"
  IFS=$'\n' read -d '' -r -a source_files < <(find_source_files && printf '\0')

  if [ ${#source_files[@]} -eq 0 ]; then
    print_warning "No source files found!"
    exit 0
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
