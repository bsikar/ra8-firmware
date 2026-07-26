#!/bin/bash
# ra8-firmware - cppcheck Static Analysis Script
#
# Runs cppcheck across libs/ and examples/ (excluding libs/third_party/) with
# the firmware-wide suppression list and, when available, the MISRA-C and
# CERT-C addons. By default findings are reported as warnings (exit 0). Pass
# --check to escalate any growth above the pinned baseline into a non-zero
# exit suitable for CI gates.
#
# Usage:
#   ./scripts/checks/cppcheck.sh             # Report findings (exit 0)
#   ./scripts/checks/cppcheck.sh --check     # Treat regressions vs baseline as error
#   ./scripts/checks/cppcheck.sh --verbose   # Verbose cppcheck output
#   ./scripts/checks/cppcheck.sh --help      # Show this help
#
# Prerequisites:
#   cppcheck >= 2.10 (macOS: brew install cppcheck;
#                     Ubuntu: sudo apt-get install cppcheck)
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT

set -euo pipefail
set +H

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

print_status() { echo -e "${BLUE}[INFO]${NC} $1" >&2; }
print_success() { echo -e "${GREEN}[SUCCESS]${NC} $1" >&2; }
print_warning() { echo -e "${YELLOW}[WARNING]${NC} $1" >&2; }
print_error() { echo -e "${RED}[ERROR]${NC} $1" >&2; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

# ---------------------------------------------------------------------------
# Baseline finding count, pinned at first run (2026-05-01).
# Any --check run that exceeds this baseline is flagged as a regression.
# Update intentionally in the same commit that fixes/adds findings.
# ---------------------------------------------------------------------------
BASELINE_TOTAL=${BASELINE_TOTAL:-CPPCHECK_BASELINE_PLACEHOLDER}

CHECK_MODE=false
VERBOSE=false

usage() {
  echo "ra8-firmware - cppcheck Static Analysis Script"
  echo ""
  echo "Usage: $0 [options]"
  echo ""
  echo "Options:"
  echo "  --check    Treat findings above the baseline as errors"
  echo "  --verbose  Verbose cppcheck output"
  echo "  --help     Show this help"
  echo ""
  echo "Default: report findings, exit 0."
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --check)
      CHECK_MODE=true
      shift
      ;;
    --verbose)
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
      exit 2
      ;;
  esac
done

if ! command -v cppcheck >/dev/null 2>&1; then
  print_error "cppcheck not found in PATH. Install with: brew install cppcheck"
  exit 2
fi

CPPCHECK_VERSION="$(cppcheck --version | awk '{print $2}')"
print_status "cppcheck version: $CPPCHECK_VERSION"

cd "$FIRMWARE_DIR"

SUPPRESSIONS="$FIRMWARE_DIR/.cppcheck-suppressions"
if [[ ! -f "$SUPPRESSIONS" ]]; then
  print_error "Suppressions file missing: $SUPPRESSIONS"
  exit 2
fi

BUILD_DIR="$FIRMWARE_DIR/build/cppcheck"
mkdir -p "$BUILD_DIR"
REPORT="$BUILD_DIR/cppcheck-report.txt"

ADDON_ARGS=()
ADDON_DIR=""
for candidate in /opt/homebrew/share/Cppcheck/addons /usr/share/cppcheck/addons /usr/local/share/Cppcheck/addons; do
  if [[ -d "$candidate" ]]; then
    ADDON_DIR="$candidate"
    break
  fi
done

if [[ -n "$ADDON_DIR" && -f "$ADDON_DIR/misra.py" ]]; then
  ADDON_ARGS+=(--addon=misra)
  print_status "MISRA addon enabled ($ADDON_DIR/misra.py)"
fi
if [[ -n "$ADDON_DIR" && -f "$ADDON_DIR/cert.py" ]]; then
  ADDON_ARGS+=(--addon=cert)
  print_status "CERT addon enabled"
else
  print_warning "CERT addon not present in this cppcheck install -- skipping"
fi

VERBOSE_ARGS=()
if $VERBOSE; then
  VERBOSE_ARGS+=(--verbose)
fi

INCLUDE_DIRS=(
  -Ilibs/ra8_core/inc
  -Ilibs/ra8_hal/inc
  -Ilibs/ra8_nsc/inc
  -Isrc/inc
  -Itools/ra8_emulator/inc
)

print_status "Running cppcheck on libs/, examples/, tools/ra8_emulator/ (excluding libs/third_party/) ..."
set +e
cppcheck \
  --enable=warning,style,performance,portability \
  --inline-suppr \
  --suppressions-list="$SUPPRESSIONS" \
  --suppress=missingIncludeSystem \
  --suppress=unmatchedSuppression \
  --suppress=*:libs/third_party/* \
  -ilibs/third_party \
  --std=c11 \
  --platform=unix32 \
  --language=c \
  --quiet \
  --error-exitcode=0 \
  "${ADDON_ARGS[@]}" \
  "${VERBOSE_ARGS[@]}" \
  "${INCLUDE_DIRS[@]}" \
  libs examples tools/ra8_emulator \
  2>"$REPORT"
RC=$?
set -e

# Count one finding per cppcheck diagnostic header. cppcheck emits
#   <file>:<line>:<col>: <severity>: <message> [<check-id>]
# followed by 2 lines of code + caret context. Match the header line
# uniquely via the trailing [<check-id>].
TOTAL=$(grep -cE ': (error|warning|style|performance|portability|information): .*\[[A-Za-z0-9_.\-]+\]$' "$REPORT" || true)

print_status "Findings written to: $REPORT"
print_status "Total findings: $TOTAL  (baseline: $BASELINE_TOTAL)"

if [[ "$TOTAL" -gt 0 ]]; then
  print_warning "Top 10 check ids by count:"
  grep -oE '\[[A-Za-z0-9_.\-]+\]$' "$REPORT" | sort | uniq -c | sort -rn | head -10 >&2 || true
fi

if $CHECK_MODE; then
  if [[ "$TOTAL" -gt "$BASELINE_TOTAL" ]]; then
    print_error "cppcheck regression: $TOTAL > baseline $BASELINE_TOTAL"
    exit 1
  fi
  if [[ "$RC" -ne 0 ]]; then
    print_error "cppcheck exited non-zero ($RC)"
    exit "$RC"
  fi
  print_success "cppcheck check passed (<= baseline)"
else
  print_success "cppcheck completed (advisory mode)"
fi

exit 0
