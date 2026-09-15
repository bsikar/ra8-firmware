# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

set working-directory := ".."

default:
    @echo "QUALITY & CI (DEVCONTAINER) - Formatters, Linters, Static Analysis & Testing"
    @echo "These commands run against your working tree in the pinned Linux devcontainer."
    @echo ""
    @echo "COMMANDS:"
    @echo "  just quality::devcontainer::gate <name>            Run a specific CI gate by name"
    @echo "  just quality::devcontainer::format                 Auto-format source code in place"
    @echo "  just quality::devcontainer::check                  Verify formatting without modifying files"
    @echo "  just quality::devcontainer::tidy                   Run clang-tidy static analysis"
    @echo "  just quality::devcontainer::cppcheck               Run cppcheck static analysis"
    @echo "  just quality::devcontainer::misra                  Run MISRA-C:2012 compliance audit"
    @echo "  just quality::devcontainer::scan_build             Run Clang Static Analyzer"
    @echo "  just quality::devcontainer::test                   Run host unit test suite"
    @echo "  just quality::devcontainer::mcdc                   Generate DO-178C MC/DC coverage report"
    @echo "  just quality::devcontainer::fuzz [seconds] [runs]  Smoke-run every libFuzzer harness"
    @echo "  just quality::devcontainer::bench                  Run host microbenchmarks"
    @echo "  just quality::devcontainer::{lint,format,test}-c   Focused C quality commands"
    @echo "  just quality::devcontainer::{lint,format,test}-go  Focused Go quality commands"
    @echo "  just quality::devcontainer::{lint,format,test}-rust Focused Rust quality commands"
    @echo "  just quality::devcontainer::{lint,format,test}-zig Focused Zig quality commands"

# Auto-format is intentionally not a CI gate: the gate checks formatting while

# this recipe writes the result back through the writable workspace mount.
format:
    bash scripts/ci/devcontainer_run.sh -- just quality::local::format

# Every non-mutating command delegates to the same native recipe CI uses. The
# only difference is transport: devcontainer_run.sh supplies the pinned Linux

# toolchain while keeping the developer's current working tree writable.
check:
    bash scripts/ci/devcontainer_run.sh -- just quality::local::check

tidy:
    bash scripts/ci/devcontainer_run.sh -- just quality::local::tidy

cppcheck:
    bash scripts/ci/devcontainer_run.sh -- just quality::local::cppcheck

misra check="0":
    bash scripts/ci/devcontainer_run.sh -- just quality::local::misra "{{ check }}"

scan_build strict="0":
    bash scripts/ci/devcontainer_run.sh -- just quality::local::scan_build "{{ strict }}"

mcdc:
    bash scripts/ci/devcontainer_run.sh -- just quality::local::mcdc

fuzz seconds="30" runs="10000":
    bash scripts/ci/devcontainer_run.sh -- just quality::local::fuzz "{{ seconds }}" "{{ runs }}"

bench:
    bash scripts/ci/devcontainer_run.sh -- just quality::local::bench

test ubsan="0":
    bash scripts/ci/devcontainer_run.sh -- just quality::local::test "{{ ubsan }}"

# Run one registered CI gate against the current working tree. This deliberately

# differs from the full `just ci` suite, which snapshots committed HEAD.
gate name:
    bash scripts/ci/devcontainer_run.sh -- just quality::local::gate "{{ name }}"

lint-c:
    bash scripts/ci/devcontainer_run.sh -- just quality::local::lint-c

format-c:
    bash scripts/ci/devcontainer_run.sh -- just quality::local::format-c

test-c:
    bash scripts/ci/devcontainer_run.sh -- just quality::local::test-c

lint-go:
    bash scripts/ci/devcontainer_run.sh -- just quality::local::lint-go

format-go:
    bash scripts/ci/devcontainer_run.sh -- just quality::local::format-go

test-go:
    bash scripts/ci/devcontainer_run.sh -- just quality::local::test-go

lint-rust:
    bash scripts/ci/devcontainer_run.sh -- just quality::local::lint-rust

format-rust:
    bash scripts/ci/devcontainer_run.sh -- just quality::local::format-rust

test-rust:
    bash scripts/ci/devcontainer_run.sh -- just quality::local::test-rust

lint-zig:
    bash scripts/ci/devcontainer_run.sh -- just quality::local::lint-zig

format-zig:
    bash scripts/ci/devcontainer_run.sh -- just quality::local::format-zig

test-zig:
    bash scripts/ci/devcontainer_run.sh -- just quality::local::test-zig
