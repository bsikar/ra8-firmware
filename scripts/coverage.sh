#!/bin/bash
# coverage.sh -- run the host unit tests with gcov instrumentation
# and produce an HTML + text coverage report.
#
# Usage:
#   ./scripts/coverage.sh
#   ./scripts/coverage.sh --gate   # exit non-zero if below 90% line / 80% branch
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FW_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$FW_DIR/build/coverage"

GATE=false
if [[ "${1:-}" == "--gate" ]]; then
    GATE=true
fi

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

if ! command -v gcovr &>/dev/null; then
    echo -e "${RED}Error:${NC} gcovr not found. Install with:"
    echo "  pip3 install gcovr"
    exit 1
fi

echo -e "${YELLOW}[1/4]${NC} Configuring coverage build..."
cmake -B "$BUILD_DIR" -S "$FW_DIR/tests" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DRA_COVERAGE=ON \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -Wno-dev >/dev/null

echo -e "${YELLOW}[2/4]${NC} Building..."
cmake --build "$BUILD_DIR" --parallel >/dev/null

echo -e "${YELLOW}[3/4]${NC} Running ctest..."
chmod +x "$BUILD_DIR"/test_* || true
ctest --test-dir "$BUILD_DIR" --output-on-failure --timeout 60 | tail -4

mkdir -p "$BUILD_DIR/coverage"
echo -e "${YELLOW}[4/4]${NC} Running gcovr..."

GCOVR_OPTS=(
    --root "$FW_DIR"
    --object-directory "$BUILD_DIR"
    --filter "$FW_DIR/libs/"
    --exclude "$FW_DIR/libs/third_party/"
    --exclude "$FW_DIR/tests/"
    --exclude-throw-branches
    --exclude-unreachable-branches
    --html-details "$BUILD_DIR/coverage/index.html"
    --txt "$BUILD_DIR/coverage/summary.txt"
    --json "$BUILD_DIR/coverage/coverage.json"
    --print-summary
)

if [[ "$GATE" == "true" ]]; then
    GCOVR_OPTS+=(
        --fail-under-line   90
        --fail-under-branch 80
    )
fi

if gcovr "${GCOVR_OPTS[@]}"; then
    echo ""
    echo -e "${GREEN}[PASS]${NC} Coverage report: $BUILD_DIR/coverage/index.html"
    cat "$BUILD_DIR/coverage/summary.txt" 2>/dev/null || true
    exit 0
else
    echo ""
    echo -e "${RED}[FAIL]${NC} Coverage gate failed"
    cat "$BUILD_DIR/coverage/summary.txt" 2>/dev/null || true
    exit 1
fi
