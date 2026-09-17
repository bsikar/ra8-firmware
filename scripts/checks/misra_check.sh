#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# ra8-firmware -- developer front end for the pinned MISRA-C 2012 audit
#
# CI and local use deliberately share one scanner implementation. The inner
# runner creates build/misra/results.txt with the repository-pinned cppcheck
# misra.py addon; --check additionally compares that result with the committed
# ratchet. Keeping this file as orchestration only prevents its addon arguments,
# source scope, and report format from drifting away from CI again.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHECK_MODE=false

usage() {
  cat <<'EOF'
ra8-firmware -- MISRA-C 2012 audit

Usage: scripts/checks/misra_check.sh [--check] [--help]

  --check  Fail when findings grow beyond .github/misra-baseline.txt.
  --help   Show this help.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --check)
      CHECK_MODE=true
      ;;
    --help | -h)
      usage
      exit 0
      ;;
    *)
      echo "misra_check.sh: unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
  shift
done

"$SCRIPT_DIR/misra_check_inner.sh"

if "$CHECK_MODE"; then
  python3 "$SCRIPT_DIR/misra_ratchet.py" --check
fi
