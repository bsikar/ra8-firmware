#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
set -euo pipefail

TEST_BIN="${1:?usage: run_workspace_test.sh <test-binary> [work-dir]}"
WORK="${2:-$(mktemp -d)}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$WORK"
python3 "$HERE/gen_corpus.py" "$WORK/corpus" >/dev/null 2>&1
"$TEST_BIN" "$WORK/corpus/legit.jof"
"$TEST_BIN" "$WORK/corpus/legit_deflate.jof"
