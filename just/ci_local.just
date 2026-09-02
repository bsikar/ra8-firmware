# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

set working-directory := ".."

default:
    @echo "QUALITY & CI (LOCAL) - Formatters, Linters, Static Analysis & Testing"
    @echo "These commands run NATIVELY on your host machine."
    @echo ""
    @echo "COMMANDS:"
    @echo "  just quality::local::gate <name>                   Run a specific CI gate natively by name"
    @echo "  just quality::local::format                        Auto-format source code in place"
    @echo "  just quality::local::check                         Verify formatting without modifying files"
    @echo "  just quality::local::tidy                          Run clang-tidy static analysis"
    @echo "  just quality::local::cppcheck                      Run cppcheck static analysis"
    @echo "  just quality::local::misra                         Run MISRA-C:2012 compliance audit"
    @echo "  just quality::local::scan_build                    Run Clang Static Analyzer"
    @echo "  just quality::local::test                          Run host unit test suite"
    @echo "  just quality::local::coverage                      Run whole-tree coverage gate"
    @echo "  just quality::local::mcdc                          Generate DO-178C MC/DC coverage report"
    @echo "  just quality::local::fuzz [seconds] [runs]         Smoke-run every libFuzzer harness"
    @echo "  just quality::local::stack_usage                   Build firmware and enforce stack budgets"
    @echo "  just quality::local::iwyu                          Run include-what-you-use (advisory)"
    @echo "  just quality::local::sbom                          Regenerate the committed CycloneDX SBOM"
    @echo "  just quality::local::sbom_check                    Verify the committed CycloneDX SBOM"
    @echo "  just quality::local::misra_baseline                Freeze MISRA burn-down on the pinned toolchain"
    @echo "  just quality::local::vela_check                    Verify the generated Vela model header"
    @echo "  just quality::local::vela_regen                    Regenerate the fake Vela model header"
    @echo "  just quality::local::vela_compile <tflite> [out]   Compile a TFLite model with Vela"
    @echo "  just quality::local::bench                         Run host microbenchmarks"
    @echo "  just quality::local::ascii                         Check ASCII encoding across files"
    @echo "  just quality::local::version                       Verify @since version annotations"

# Run code formatters in-place
format:
    bash scripts/checks/format_code.sh

# Verify formatting without modifying files
check:
    bash scripts/checks/format_code.sh --check

# Run clang-tidy through the registered gate with CI/ratchet return semantics
tidy:
    "{{ just_executable() }}" quality::local::gate tidy

# Run cppcheck static analysis
cppcheck:
    /bin/bash -p scripts/checks/cppcheck.sh

# Check file encodings for non-ASCII characters
ascii:
    python3 scripts/fix/fix-encoding.py --selftest
    python3 scripts/fix/fix-encoding.py --check --all

# Verify @since version tags
version:
    python3 scripts/checks/check-since-version.py --all

# Run host unit tests
test ubsan="0":
    #!/usr/bin/env bash
    set -euo pipefail
    if [ "{{ ubsan }}" = "1" ]; then
        bash tests/build_tests.sh --ubsan
        bash tests/run_tests.sh --ubsan
    else
        bash tests/build_tests.sh
        bash tests/run_tests.sh
    fi

# Generate DO-178C Level B MC/DC coverage report
mcdc:
    CC=clang-18 CXX=clang++-18 RA8_MCDC_THRESHOLD=0 bash scripts/report/mcdc_report.sh

# Keep this welded to the registered gate so its self-tests and compiler pin
# cannot drift from CI.

# Measure and enforce the whole-tree line/branch coverage ratchet
coverage:
    /bin/bash -p scripts/ci.sh --gate coverage-tree

# Smoke-run every registered libFuzzer harness with the former local defaults.
fuzz seconds="30" runs="10000" cc="" cxx="":
    #!/usr/bin/env bash
    set -euo pipefail
    bash scripts/checks/run_fuzz.sh --selftest
    CC="{{ cc }}" CXX="{{ cxx }}" FUZZ_RUNS="{{ runs }}" \
        bash scripts/checks/run_fuzz.sh --all "{{ seconds }}"

# Build every firmware app before reading the emitted .su files. Keeping both

# phases on registered gates preserves their toolchain and non-vacuity checks.
stack_usage:
    "{{ just_executable() }}" quality::local::gate build-cross
    "{{ just_executable() }}" quality::local::gate stack-usage

alias stack-usage := stack_usage

# Run MISRA-C:2012 audit
misra check="0":
    #!/usr/bin/env bash
    set -euo pipefail
    bash scripts/checks/misra_check_inner.sh
    if [ "{{ check }}" = "1" ]; then
        python3 scripts/checks/misra_ratchet.py --check
    fi

# Never use this to accept new findings; the recipe intentionally runs the
# same producer first.

# Regenerate the MISRA baseline after an audited burn-down
misra_baseline:
    bash scripts/checks/misra_check_inner.sh
    python3 scripts/checks/misra_ratchet.py --update

alias misra-baseline := misra_baseline

# Run include-what-you-use. This remains advisory, matching the former target.
iwyu:
    bash scripts/checks/iwyu.sh

# Regenerate or verify the committed third-party CycloneDX inventory.
sbom:
    python3 scripts/gen/gen_sbom.py

sbom_check:
    /bin/bash -p scripts/ci.sh --gate sbom

alias sbom-check := sbom_check

# Verify, regenerate, or compile an Arm Ethos-U Vela model artifact.
vela_check:
    python3 tools/vela/src/vela_gen.py check tools/vela/models/npu_addk_fake.json tools/vela/generated/ra8_npu_model_addk_fake.h

alias vela-check := vela_check

vela_regen:
    python3 tools/vela/src/vela_gen.py emit tools/vela/models/npu_addk_fake.json -o tools/vela/generated/ra8_npu_model_addk_fake.h

alias vela-regen := vela_regen

vela_compile tflite out="build/vela":
    python3 tools/vela/src/vela_gen.py compile "{{ tflite }}" -o "{{ out }}"

alias vela-compile := vela_compile

# Run Clang Static Analyzer (scan-build)
scan_build strict="0":
    #!/usr/bin/env bash
    set -euo pipefail
    if [ "{{ strict }}" = "1" ]; then
        bash scripts/checks/scan_build.sh --strict
    else
        bash scripts/checks/scan_build.sh
    fi

alias scan-build := scan_build

# Run host microbenchmarks
bench:
    #!/usr/bin/env bash
    set -euo pipefail
    bash scripts/builders/host_cmake.sh --target ra8_bench_all tests tests/build-bench -DRA8_BENCH=ON
    for benchmark in bench_ra8_crc bench_ra8_jpeg_sw bench_ra8_gfx_text; do
        "tests/build-bench/${benchmark}"
    done

# Run a specific CI gate natively by name
gate name:
    #!/usr/bin/env bash
    set -euo pipefail
    /bin/bash -p scripts/ci.sh --gate "{{ name }}"
