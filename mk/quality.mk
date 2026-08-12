# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# mk/quality.mk -- format, lint, tests, coverage, MISRA, static analysis, CI.
# Shared vars (TESTS_DIR, TESTS_BUILD*, EK_APPS, CLANG_FORMAT, ...) come from the
# top Makefile.

.PHONY: format check tidy cppcheck magic ascii version \
        test test-docker ubsan test-cov ctest coverage mcdc \
        misra misra-check misra-baseline scan-build scan-build-strict iwyu \
        fuzz bench stack-usage check-annotations nsc-cmse-check \
        sbom sbom-check soup-check soup-refresh \
        vela vela-check vela-regen vela-compile \
        ci ci-fast ci-native ci-native-fast ci-list ci-gate ci-gate-container

format:
	bash scripts/checks/format_code.sh

check:
	bash scripts/checks/format_code.sh --check

tidy:
	bash scripts/checks/clang_tidy.sh --check

# `make cppcheck` -- local parity with the CI cppcheck gate.
cppcheck:
	bash scripts/checks/cppcheck.sh

# `make magic` -- full-tree magic-number gate (backstops clang-tidy's
# readability-magic-numbers, which only sees files in the host compile-db).
magic:
	python3 scripts/checks/check_magic_numbers.py

# Scope is DERIVED from git ls-files (--all), never a directory list. The two
# loops that used to live here missed the repo root, .github/, cmake/,
# coprocessor/, infra/ and mk/ -- 106 files including CLAUDE.md itself (#533).
# Identical to what `bash scripts/ci.sh --gate ascii` runs.
ascii:
	python3 scripts/fix/fix-encoding.py --selftest
	python3 scripts/fix/fix-encoding.py --check --all

version:
	@echo "project VERSION: $$(cat VERSION)"
	@python3 scripts/checks/check-since-version.py --all

# Host unit tests (tests/build/). On macOS the firmware host tests SIGKILL
# (MAP_FIXED below 4 GiB) -- use `make ci` / `make test-docker` for a faithful run.
test:
	bash $(TESTS_DIR)/build_tests.sh
	bash $(TESTS_DIR)/run_tests.sh

test-docker:
	bash scripts/ci/test-docker.sh

# `make ubsan` -- host suite under UBSan (separate tests/build-ubsan/ tree).
ubsan:
	bash $(TESTS_DIR)/build_tests.sh --ubsan
	bash $(TESTS_DIR)/run_tests.sh --ubsan

# `make test-cov` is an alias for `make mcdc` (coverage build tree).
test-cov: mcdc

ctest:
	ctest --test-dir $(TESTS_BUILD) --output-on-failure

coverage:
	bash scripts/report/coverage_report.sh
	python3 scripts/checks/check_coverage.py

# Use the SAME mcdc_report.sh invocation the `mcdc` CI gate uses
# (scripts/ci/gates/tests.sh), so "run the documented target" and "run what CI
# runs" cannot diverge (#556). Two pins matter, both taken from the gate:
#   CC=clang-18 CXX=clang++-18 -- mcdc_report.sh needs clang >= 18 for
#     -fcoverage-mcdc; bare `make mcdc` would inherit the ambient `cc` (gcc-12
#     on the shared dev box), which cannot produce MC/DC data at all.
#   RA8_MCDC_THRESHOLD=0 -- disables mcdc_report.sh's own 100%-reachable
#     aggregate gate. That is NOT the project's quality bar (the gate states so
#     and applies the .github/mcdc-baseline.txt comparison instead); leaving it
#     on makes `make mcdc` FAIL on a tree CI reports green, the exact dev-box
#     red #557 warns people learn to ignore.
# llvm-profdata-18 / llvm-cov-18 resolve beside clang-18. macOS has no clang-18
# and cannot run the host suite regardless.
mcdc:
	CC=clang-18 CXX=clang++-18 RA8_MCDC_THRESHOLD=0 bash scripts/report/mcdc_report.sh

# MISRA-C 2012 audit (see docs/MISRA.md).
misra:
	bash scripts/checks/misra_check_inner.sh

misra-check: misra
	python3 scripts/checks/misra_ratchet.py --check

misra-baseline: misra
	python3 scripts/checks/misra_ratchet.py --update

# Clang Static Analyzer (scan-build). Strict variant is the CI-gating entry.
scan-build:
	bash scripts/checks/scan_build.sh

scan-build-strict:
	bash scripts/checks/scan_build.sh --strict

# include-what-you-use (IWYU). Warn-only today.
iwyu:
	bash scripts/checks/iwyu.sh

# `make fuzz` -- build every libFuzzer harness (clang only) + smoke-run each.
FUZZ_SECONDS ?= 30
FUZZ_RUNS    ?= 10000
FUZZ_CC      ?=
FUZZ_CXX     ?=
fuzz:
	@bash scripts/checks/run_fuzz.sh --selftest
	@CC="$(FUZZ_CC)" CXX="$(FUZZ_CXX)" FUZZ_RUNS="$(FUZZ_RUNS)" \
	  bash scripts/checks/run_fuzz.sh --all "$(FUZZ_SECONDS)"

# `make bench` -- host-side performance microbenchmarks (opt-in -DRA8_BENCH=ON,
# own build tree). See docs/PERFORMANCE.md.
BENCH_BUILD   := $(TESTS_DIR)/build-bench
BENCH_TARGETS := bench_ra8_crc bench_ra8_jpeg_sw bench_ra8_gfx_text
bench:
	$(CMAKE) -S $(TESTS_DIR) -B $(BENCH_BUILD) \
	    -DRA8_BENCH=ON -DRA8_COVERAGE=OFF -DRA8_MCDC=OFF \
	    -DCMAKE_BUILD_TYPE=Release \
	    -Wno-dev
	$(CMAKE) --build $(BENCH_BUILD) --target ra8_bench_all -j
	@echo ""
	@echo "==== ra8_bench results ===="
	@for t in $(BENCH_TARGETS); do \
	    echo "---- $$t ----"; \
	    $(BENCH_BUILD)/bench/$$t || { echo "FAIL: $$t"; exit 1; }; \
	done
	@echo "==== ra8_bench done ===="

# Stack-usage proof: build every EVM app + aggregate the -fstack-usage report.
stack-usage: $(EK_APPS)
	python3 scripts/checks/stack_usage_check.py --top 10

# Annotation-attribute enforcement (the 19 ra8_* rules; docs/ANNOTATIONS.md).
check-annotations:
	python3 scripts/checks/check_annotations.py --check

# `make nsc-cmse-check` -- compile every libs/ra8_nsc veneer under -mcmse.
nsc-cmse-check:
	bash scripts/checks/check_nsc_cmse.sh

# Third-party SBOM (docs/sbom/) regenerate + drift gate.
sbom:
	python3 scripts/gen/gen_sbom.py

sbom-check:
	python3 scripts/gen/gen_sbom.py --check

# Vendored SOUP vs the upstream projects that published it (issue #548).
# `soup-check` is offline -- it compares our index against the blob hashes
# committed under docs/sbom/upstream/. `soup-refresh` NEEDS THE NETWORK: it
# re-fetches every pinned upstream revision and rewrites those manifests, and
# is the only way to record a newly declared patch or a re-vendored component.
soup-check:
	python3 scripts/checks/check_soup_upstream.py --selftest
	python3 scripts/checks/check_soup_upstream.py

soup-refresh:
	python3 scripts/checks/check_soup_upstream.py --refresh

# `make vela-check` -- regenerate the Ethos-U55 model header + diff vs the golden
# (issue #227; no Vela toolchain needed). See tools/vela/README.md.
VELA_GEN    := python3 tools/vela/vela_gen.py
VELA_DESC   := tools/vela/models/npu_addk_fake.json
VELA_HEADER := tools/vela/generated/ra8_npu_model_addk_fake.h
vela: vela-check
vela-check:
	$(VELA_GEN) check $(VELA_DESC) $(VELA_HEADER)
vela-regen:
	$(VELA_GEN) emit $(VELA_DESC) -o $(VELA_HEADER)
vela-compile:
	$(VELA_GEN) compile $(TFLITE) -o $(or $(VELA_OUT),build/vela)

# `make ci` -- run every CI gate before pushing.
#
# The gate bodies live in scripts/ci.sh and the workflows call the SAME
# functions (`bash scripts/ci.sh --gate <name>`), so this cannot validate a
# different set of checks than the runner does. That was the recurring failure:
# the annotation gate and the MISRA ratchet were each absent from a
# hand-maintained local copy, so a full local run passed while CI went red.
# `make ci-list` prints the registry; `make ci-gate GATE=<name>` runs one.
#
# `make ci` containerises (needed on macOS: the host tests mmap MAP_FIXED below
# 4 GiB, which macOS arm64 refuses). On Linux with no container runtime it
# falls back to running natively, because Linux native IS the CI environment.
# `make ci-native` asks for that path explicitly -- use it on a box with no
# docker/podman. Both run against a clean snapshot of committed HEAD, so
# in-source build junk and stale .gcda cannot skew a gate.
# The container image refreshes ITSELF: it carries a sha256 of the
# .devcontainer/ build context as a label, and a cached image whose label
# disagrees with the working tree is rebuilt rather than booted (#521). REBUILD=1
# forces a build even when it agrees -- it is no longer how you avoid running the
# gates against a stale toolchain.
ci:
	bash scripts/ci.sh $(if $(filter-out 0,$(REBUILD)),--rebuild,)

ci-fast:
	bash scripts/ci.sh --fast $(if $(filter-out 0,$(REBUILD)),--rebuild,)

ci-native:
	bash scripts/ci.sh --native

ci-native-fast:
	bash scripts/ci.sh --native --fast

ci-list:
	@bash scripts/ci.sh --list-gates

ci-gate:
	@test -n "$(GATE)" || { echo "usage: make ci-gate GATE=<name>  (make ci-list to see them)" >&2; exit 2; }
	bash scripts/ci.sh --gate $(GATE)

# One gate, but in the toolchain container instead of on this host.
#
# `make ci-gate` is the invocation CI itself uses, so it runs the gate NATIVELY
# and assumes the host is a CI-equivalent environment. Two hosts deliberately
# are not: macOS cannot be, and a runner host whose only toolchain is the image
# its runners boot is not either -- installing a second toolchain beside it is
# exactly the drift the shared image exists to prevent. This runs the same gate
# on the same clean HEAD snapshot, inside that image.
ci-gate-container:
	@test -n "$(GATE)" || { echo "usage: make ci-gate-container GATE=<name>" >&2; exit 2; }
	bash scripts/ci.sh --container --gate $(GATE)
