#!/bin/bash -p
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SHEBANG-SECURITY: -p blocks BASH_ENV and exported-function startup injection.
# ra8-firmware - cppcheck Static Analysis Script
#
# Runs cppcheck across libs/, examples/, and tools/ (excluding libs/third_party/) with
# the firmware-wide suppression list and cppcheck's deterministic built-in rule
# set. Optional addons are deliberately excluded: MISRA has a separate producer
# and per-file/per-rule ratchet, while host-installed addon availability must
# never change this gate's meaning. By default findings are reported as warnings
# (exit 0). Pass --check to escalate growth above the pinned baseline into a
# non-zero exit suitable for CI gates.
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

if [[ "$-" == *p* ]]; then
  unset -v BASH_ENV ENV
  declare -a ra8_startup_env_unset=()
  _ra8_startup_refuse() {
    printf 'error: privileged startup %s\n' "$1" >&2
    exit 1
  }
  ra8_startup_env_done_count=0
  while IFS= read -r -d '' ra8_startup_env_row; do
    ra8_startup_env_name="${ra8_startup_env_row%%=*}"
    case "$ra8_startup_env_name" in
      RA8_STARTUP_ENV_DONE)
        ra8_startup_env_done_count=$((ra8_startup_env_done_count + 1))
        ;;
      BASH_FUNC_*%% | BASH_FUNC_*'()') ra8_startup_env_unset+=(-u "$ra8_startup_env_name") ;;
    esac
  done < <(
    /usr/bin/env -u RA8_STARTUP_ENV_DONE -0 &&
      /usr/bin/printf 'RA8_STARTUP_ENV_DONE=1\0'
  )
  ((ra8_startup_env_done_count == 1)) && [[ "$ra8_startup_env_name" == RA8_STARTUP_ENV_DONE ]] || _ra8_startup_refuse 'environment enumeration was incomplete'
  if ((${#ra8_startup_env_unset[@]})); then
    [[ -z "${RA8_STARTUP_ENV_SCRUBBED-}" ]] || _ra8_startup_refuse 'scrub did not converge'
    ra8_startup_reentry="$0"
    [[ "$ra8_startup_reentry" == */* ]] || _ra8_startup_refuse 'requires a script path'
    if [[ "$ra8_startup_reentry" != /* ]]; then
      ra8_startup_reentry="$PWD/$ra8_startup_reentry"
    fi
    ra8_startup_check="$ra8_startup_reentry"
    while [[ "$ra8_startup_check" != "/" ]]; do
      [[ ! -L "$ra8_startup_check" ]] || _ra8_startup_refuse 'refuses a symlinked path'
      ra8_startup_parent="${ra8_startup_check%/*}"
      [[ -n "$ra8_startup_parent" ]] || ra8_startup_parent="/"
      [[ "$ra8_startup_parent" != "$ra8_startup_check" ]] ||
        _ra8_startup_refuse 'cannot validate its script path'
      ra8_startup_check="$ra8_startup_parent"
    done
    [[ -f "$ra8_startup_reentry" ]] || _ra8_startup_refuse 'refuses a non-regular path'
    if ! exec /usr/bin/env "${ra8_startup_env_unset[@]}" -u BASH_ENV -u ENV \
      -u RA8_STARTUP_ENV_DONE RA8_STARTUP_ENV_SCRUBBED=1 \
      /bin/bash -p -- "$ra8_startup_reentry" "$@"; then
      _ra8_startup_refuse 'could not enter sanitized process'
    fi
  fi
  unset -v ra8_startup_check ra8_startup_env_done_count
  unset -v ra8_startup_env_name ra8_startup_env_row
  unset -v ra8_startup_env_unset ra8_startup_parent ra8_startup_reentry
  unset -v RA8_STARTUP_ENV_DONE
  unset -v RA8_STARTUP_ENV_SCRUBBED
  unset -f _ra8_startup_refuse

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
  readonly BASELINE_TOTAL=0

  # ---------------------------------------------------------------------------
  # The include scope, and the ONLY definition of it.
  #
  # cppcheck analyses translation units, and a TU whose includes do not resolve
  # is not the TU that ships: the annotation macros in ra8_attributes.h go
  # unresolved, and a function-like one at file scope --
  # `RA8_OWNS_RESOURCE("...")` before a definition -- stops looking like an
  # annotation and starts looking like a call, which cppcheck reports as
  # `unknownMacro`. The pre-commit hook re-derived its own (empty) include list
  # and hit exactly that on the first such annotation to land, while this gate
  # was green on the identical tree. So the list is published through
  # --print-include-dirs and the hook consumes it rather than keeping a second
  # copy that can drift again.
  # ---------------------------------------------------------------------------
  INCLUDE_DIRS=(
    -Ilibs/ra8_core/inc
    -Ilibs/ra8_hal/inc
    -Ilibs/ra8_nsc/inc
    -Itools/ra8_emulator/inc
    -Iapps/shared_libs/mdl/inc
    -Iapps/host/mdl/inc
    -Itools/rabook_viewer/inc
    -Itools/rabook_imagepack/inc
  )

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
    echo "  --print-include-dirs  Print the include scope, one -I per line, and exit"
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
      --print-include-dirs)
        printf '%s\n' ${INCLUDE_DIRS[@]+"${INCLUDE_DIRS[@]}"}
        exit 0
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

  # Reuse the repository's fixed Python/Git authorities. The helper is always
  # launched in isolated mode, so inherited PYTHONPATH/sitecustomize and Git
  # routing/configuration cannot replace the source census.
  # shellcheck source=scripts/dev/git_environment.sh
  . "$FIRMWARE_DIR/scripts/dev/git_environment.sh"

  run_census_python() {
    /usr/bin/env -u BASH_ENV -u ENV -u PYTHONHOME -u PYTHONPATH \
      -u PYTHONSTARTUP -u PYTHONINSPECT \
      "$RA8_TRUSTED_PYTHON" -I -S scripts/checks/cppcheck_sources.py "$@"
  }

  SUPPRESSIONS="$FIRMWARE_DIR/.cppcheck-suppressions"
  if [[ ! -f "$SUPPRESSIONS" ]]; then
    print_error "Suppressions file missing: $SUPPRESSIONS"
    exit 2
  fi

  BUILD_DIR="$FIRMWARE_DIR/build/cppcheck"
  mkdir -p "$BUILD_DIR"
  REPORT="$BUILD_DIR/cppcheck-report.txt"

  # Consume the same authenticated NUL manifest as the registered gate. The
  # second isolated process independently validates transport order, uniqueness,
  # scope, filesystem confinement, and the non-vacuity floor.
  RAW_MANIFEST="$(mktemp "${TMPDIR:-/tmp}/ra8-cppcheck-raw.XXXXXXXX")"
  SOURCE_MANIFEST="$(mktemp "${TMPDIR:-/tmp}/ra8-cppcheck-sources.XXXXXXXX")"
  trap 'rm -f -- "$RAW_MANIFEST" "$SOURCE_MANIFEST"' EXIT
  run_census_python --selftest
  run_census_python --null >"$RAW_MANIFEST"
  run_census_python --validate-manifest "$RAW_MANIFEST" --null >"$SOURCE_MANIFEST"
  SOURCE_FILES=()
  while IFS= read -r -d '' source; do
    SOURCE_FILES+=("$source")
  done <"$SOURCE_MANIFEST"
  if [[ "${#SOURCE_FILES[@]}" -eq 0 ]]; then
    print_error "validated cppcheck source manifest transported zero units"
    exit 2
  fi
  print_status "Validated cppcheck source census: ${#SOURCE_FILES[@]} translation units"

  VERBOSE_ARGS=()
  if $VERBOSE; then
    VERBOSE_ARGS+=(--verbose)
  fi

  print_status "Running cppcheck on libs/, examples/, tools/ (excluding libs/third_party/) ..."
  set +e
  cppcheck \
    --enable=warning,style,performance,portability \
    --inline-suppr \
    --include=scripts/checks/cppcheck_c23_compat.h \
    --suppressions-list="$SUPPRESSIONS" \
    --suppress=missingIncludeSystem \
    --max-configs=64 \
    --suppress=unmatchedSuppression \
    --suppress=*:libs/third_party/* \
    -ilibs/third_party \
    --suppress=*:apps/shared_libs/third_party/* \
    -iapps/shared_libs/third_party \
    --std=c11 \
    --platform=unix32 \
    --quiet \
    --error-exitcode=1 \
    ${VERBOSE_ARGS[@]+"${VERBOSE_ARGS[@]}"} \
    ${INCLUDE_DIRS[@]+"${INCLUDE_DIRS[@]}"} \
    ${SOURCE_FILES[@]+"${SOURCE_FILES[@]}"} \
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
    if [[ "$TOTAL" -eq 0 ]] && [[ "$RC" -ne 0 ]]; then
      # Safety net: if we expect zero findings but cppcheck still errored,
      # something is wrong with the scan itself.
      print_error "cppcheck exited non-zero ($RC) despite zero findings"
      exit "$RC"
    fi
    print_success "cppcheck check passed (<= baseline)"
  else
    print_success "cppcheck completed (advisory mode)"
  fi

  exit 0
else
  [[ "$-" == *p* ]]
fi
