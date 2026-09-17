# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Command surface for the root Zig build graph (#857), the prerequisite for
# retiring CMake (#859). Every recipe here dispatches to a step `build.zig`
# already owns: no compiler flags, include paths or warning suppressions live
# in this file, because a command layer that starts assembling compiler
# arguments has re-created the problem this migration is trying to retire.
#
# CMake stays authoritative. The CMake-backed recipes (`just tests::build`,
# `just apps::build`, `just apps::compile_commands`) are untouched, so both
# surfaces can be run on the same machine and compared.

set working-directory := ".."

# Zig toolchain. `zig` on PATH by default, the same resolution
# tests/cmake/zig_library.cmake and cmake/ra8_app/zig_libs.cmake perform with
# find_program(); RA8_ZIG names a pinned or out-of-PATH install instead.

export RA8_ZIG := env('RA8_ZIG', 'zig')

# Show the Zig build-graph help menu
default:
    @echo ""
    @echo "ZIG - Root Zig build graph (the migrated slice of the CMake build)"
    @echo ""
    @echo "COMMANDS:"
    @echo "  just zig::build             Build the migrated Zig archives"
    @echo "  just zig::test              Run every suite in the slice"
    @echo "  just zig::test_c            Run the unmodified C suites only"
    @echo "  just zig::test_zig          Run the Zig-native suites only"
    @echo "  just zig::test_soup         Run the vendored xz-embedded suite"
    @echo "  just zig::arm               Cross-build one example app for the RA8D2"
    @echo "  just zig::compile_commands  Emit the graph's compile_commands.json"
    @echo "  just zig::analysis          Prove every compile command still compiles"
    @echo "  just zig::shapes            Check the app-shape ledger against the tree"
    @echo "  just zig::abi               Prove the Zig-to-C ABI contract"
    @echo "  just zig::parity            Print the slice manifest"
    @echo "  just zig::clean             Remove this graph's build outputs"
    @echo ""
    @echo "The CMake path is unchanged: just tests::build, just apps::build,"
    @echo "just apps::compile_commands. Both surfaces build the same sources."
    @echo ""

# Fail closed when no Zig toolchain is reachable. CMake hard-errors in the same
# situation (`migrated Zig library requires zig on PATH`), so a recipe that

# skipped instead would report a pass for a build that never happened.
_zig:
    #!/usr/bin/env bash
    set -euo pipefail
    if ! command -v "${RA8_ZIG}" >/dev/null 2>&1; then
        echo "error: no Zig toolchain found as '${RA8_ZIG}'." >&2
        echo "       Install the pinned Zig 0.14.1, or set RA8_ZIG to its path." >&2
        exit 1
    fi

# Build the migrated Zig archives of the slice
build: _zig
    "${RA8_ZIG}" build

# Build and run every suite in the slice: Zig-native, C, and vendored
test: _zig
    "${RA8_ZIG}" build test

# Run the unmodified C acceptance suites against the Zig archives
test_c: _zig
    "${RA8_ZIG}" build test-c

# Run the Zig-native unit suites of the migrated libraries
test_zig: _zig
    "${RA8_ZIG}" build test-zig

# Compile the vendored xz-embedded decoder and run its first-party suite
test_soup: _zig
    "${RA8_ZIG}" build test-soup

# Cross-build one example application for the RA8D2 target
arm: _zig
    "${RA8_ZIG}" build arm

# Emit zig-out/analysis/compile_commands.json for the TUs this graph compiles
compile_commands: _zig
    "${RA8_ZIG}" build compile-db

# Re-run every distinct compile command of that database against its own TU
analysis: _zig
    "${RA8_ZIG}" build analysis

# Hold the committed app-shape ledger to the tree's ra8_add_app() declarations
shapes: _zig
    "${RA8_ZIG}" build shapes

# Prove the Zig-to-C ABI contract of the migrated archives, negative controls included
abi: _zig
    "${RA8_ZIG}" build abi

# Print the slice manifest the CMake-side parity check reads
parity: _zig
    "${RA8_ZIG}" build parity

# Remove this graph's own build outputs; no CMake tree is touched
clean:
    rm -rf zig-out .zig-cache
