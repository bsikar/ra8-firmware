# mk/quality.mk -- format, lint, tests, coverage, MISRA, static analysis, CI.
# Shared vars (TESTS_DIR, TESTS_BUILD*, EK_APPS, CLANG_FORMAT, ...) come from the
# top Makefile.
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT

.PHONY: format check tidy cppcheck magic ascii version \
        test test-docker ubsan test-cov ctest coverage mcdc \
        misra misra-check misra-baseline scan-build scan-build-strict iwyu \
        fuzz bench stack-usage check-annotations nsc-cmse-check \
        sbom sbom-check vela vela-check vela-regen vela-compile \
        ci ci-fast ci-native ci-native-fast ci-list ci-gate

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

ascii:
	@for dir in src libs tests examples port scripts tools docs; do \
		python3 scripts/fix/fix-encoding.py --check "$$dir" || exit 1; \
	done
	@for d in $(ROOT)/examples/*/*/main.c $(ROOT)/examples/*/*/*/main.c $(ROOT)/examples/*/*/*/*/main.c; do \
		[ -f "$$d" ] || continue; \
		python3 scripts/fix/fix-encoding.py --check "$$(dirname $$d)" || exit 1; \
	done

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

mcdc:
	bash scripts/report/mcdc_report.sh

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
# docker/podman. Both run against a clean `git archive HEAD` snapshot, so
# in-source build junk and stale .gcda cannot skew a gate.
# REBUILD=1 forces a fresh image.
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
