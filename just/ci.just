# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

set working-directory := ".."

default:
    @echo "QUALITY & CI - Formatters, Linters, Static Analysis & Testing"
    @echo ""
    @echo "SUBMODULES:"
    @echo "  just quality::local                          Run tools NATIVELY on your host machine"
    @echo "  just quality::devcontainer                   Run tools ISOLATED inside the devcontainer"
    @echo "  just quality::gate                           CI Gate metadata and operations"
    @echo ""
    @echo "CI WORKFLOW DRIVERS:"
    @echo "  just quality::run                            Run full CI gate suite"
    @echo "  just quality::fast                           Run fast local CI gates"

# --- Toolchain Resolution ---
# format_code.sh owns the formatter default and validates its major so direct
# CI invocation cannot diverge from these recipes. Just supplies Homebrew's
# unversioned v22 path on macOS and preserves an explicit caller override.

export CLANG_FORMAT := env('CLANG_FORMAT', `if command -v clang-format-22 >/dev/null 2>&1; then command -v clang-format-22; elif [ "$(uname -s)" = "Darwin" ] && command -v clang-format >/dev/null 2>&1; then command -v clang-format; else echo "clang-format-22"; fi`)

# The Dockerfile-derived registry owns the tidy major. An explicit override may
# select another path, but clang_tidy.sh still verifies its reported major.

export CLANG_TIDY := env('CLANG_TIDY', `python3 scripts/checks/check_tool_versions.py --print-binary clang-tidy`)

mod local 'ci_local.just'
mod devcontainer 'ci_devcontainer.just'
mod gate 'ci_gate.just'

run:
    /bin/bash -p scripts/ci.sh

fast:
    /bin/bash -p scripts/ci.sh --fast

native:
    /bin/bash -p scripts/ci.sh --native

native_fast:
    /bin/bash -p scripts/ci.sh --native --fast

alias native-fast := native_fast
