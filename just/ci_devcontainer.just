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

alias scan-build := scan_build

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
