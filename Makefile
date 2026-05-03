#
# Top-level Makefile for ra8d2-firmware.
#
# Each application lives in its own directory under
# examples/<tier>/<app>/ with its own main.c, boot files
# (vector_table.c, system_init.c, secure_exception.c,
# trustzone_init.c), linker_script.ld, Makefile, and CMakeLists.txt.
# The <tier> layer groups apps by hardware-support category
# (ek_ra8d2/ for the stock EVM, _unsupported/ for apps needing extra
# hardware). The build-target name is just the bare app dir name --
# the tier directory is purely organisational, so `make blink` works
# regardless of which tier blink/ lives in. This top-level Makefile
# auto-discovers them and forwards `make <app>` to the per-app
# Makefile.
#
# Common targets:
#
#   make            -- build the default app ($(RA_DEFAULT_APP))
#   make <app>      -- build a specific app, e.g. `make blink_hal`
#   make apps       -- list every discovered app
#   make clean      -- remove every app build dir and tests/build
#   make format     -- run clang-format --in-place on the whole tree
#   make check      -- run clang-format --dry-run (no changes)
#   make tidy       -- run clang-tidy against the whole tree
#   make test       -- build + run the host unit tests
#   make docs       -- generate doxygen HTML into build/docs/html/
#   make dashboard  -- regenerate docs/ROADMAP_DASHBOARD.md + docs/badges/
#   make ascii      -- scan for non-ASCII characters
#   make version    -- verify @since tags match the VERSION file
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

SHELL := /bin/bash

ROOT             := $(abspath .)
TESTS_DIR        := $(ROOT)/tests
TESTS_BUILD      := $(TESTS_DIR)/build
TESTS_BUILD_COV  := $(TESTS_DIR)/build-cov
# Legacy tidy build dir kept so `bash scripts/clang_tidy.sh` keeps working.
TIDY_BUILD       := $(ROOT)/build/tidy

CMAKE        ?= cmake
CLANG_FORMAT ?= clang-format
DOXYGEN      ?= doxygen
ARM_SIZE     ?= arm-none-eabi-size

# Default app -- override on the command line, e.g. `make RA_DEFAULT_APP=blink_hal`.
RA_DEFAULT_APP ?= blink

# Auto-discover apps: every examples/<tier>/<name>/ dir with main.c +
# CMakeLists.txt. RA_APPS holds the bare app names (e.g. "blink");
# RA_APP_DIR_<app> resolves each one to its full per-app directory so
# `make blink` works regardless of which tier (ek_ra8d2/ vs
# _unsupported/) the app lives in. A "tier" is the hardware-support
# category, organised so a developer can see at a glance which apps
# we can hardware-validate on the stock EK-RA8D2 EVM.
_RA_APP_MAINS := $(wildcard $(ROOT)/examples/*/*/main.c)
RA_APPS       := $(sort $(notdir $(patsubst %/main.c,%,$(_RA_APP_MAINS))))
$(foreach m,$(_RA_APP_MAINS),$(eval RA_APP_DIR_$(notdir $(patsubst %/main.c,%,$m)) := $(patsubst %/main.c,%,$m)))

# We forward each <app> name to the app's own Makefile, so reserve the names
# below from being shadowed by .PHONY targets.
.PHONY: help apps default clean format check tidy test test-cov test-docker ctest coverage mcdc misra docs dashboard ascii version qe-test smoke stack-usage scan-build scan-build-strict iwyu fuzz bench app-sizes check-annotations all $(RA_APPS)

# EVM-tier apps (everything under examples/ek_ra8d2/) -- these are the
# apps the hardware smoke test sweeps because they run on a stock
# EK-RA8D2 with no extra peripherals required.
_EK_APP_MAINS := $(wildcard $(ROOT)/examples/ek_ra8d2/*/main.c)
EK_APPS       := $(sort $(notdir $(patsubst %/main.c,%,$(_EK_APP_MAINS))))

help:
	@echo "ra8d2-firmware make targets:"
	@echo "  make           build the default app ($(RA_DEFAULT_APP))"
	@echo "  make <app>     build a specific app -- one of: $(RA_APPS)"
	@echo "  make apps      list every discovered app"
	@echo "  make clean     remove every app build dir and tests/build"
	@echo "  make format    run clang-format in place"
	@echo "  make check     run clang-format --dry-run"
	@echo "  make tidy      run clang-tidy"
	@echo "  make test      host-compile + run unit tests (tests/build/)"
	@echo "  make test-cov  alias for make mcdc (tests/build-cov/)"
	@echo "  make test-docker host-compile + run unit tests in Linux container"
	@echo "  make ctest     rerun just ctest (assumes already built)"
	@echo "  make coverage  generate lcov+genhtml HTML coverage report"
	@echo "  make mcdc      generate DO-178C Level B MC/DC report (clang >= 18)"
	@echo "  make misra     run MISRA-C 2012 audit (advisory; see docs/MISRA.md)"
	@echo "  make docs      generate doxygen HTML into build/docs/html/"
	@echo "  make dashboard regenerate docs/ROADMAP_DASHBOARD.md + docs/badges/"
	@echo "  make ascii     fix-encoding.py --check"
	@echo "  make version   verify @since tags match the VERSION file"
	@echo "  make qe-test   run tools/ra_qe (JSON configurator) unit tests"
	@echo "  make smoke     hardware smoke-test sweep over examples/ek_ra8d2/"
	@echo "  make stack-usage build EVM apps + aggregate -fstack-usage report"
	@echo "  make scan-build run clang static analyzer over the host test build"
	@echo "  make iwyu      run include-what-you-use over the host test build"
	@echo "  make fuzz      build + smoke-run libFuzzer harnesses (clang only)"
	@echo "  make audit-init per-app init-order audit (writes docs/INIT_ORDER_AUDIT.md)"

# `make` with no arg builds the default app.
default: $(RA_DEFAULT_APP)

apps:
	@echo "Available apps (grouped by tier):"
	@for tier_dir in $(ROOT)/examples/*/; do \
		tier=$$(basename "$$tier_dir"); \
		echo "  [$$tier]"; \
		for main in "$$tier_dir"*/main.c; do \
			[ -f "$$main" ] || continue; \
			app=$$(basename "$$(dirname "$$main")"); \
			first_brief=$$(grep -m1 "@brief" "$$main" 2>/dev/null | sed 's/.*@brief //'); \
			printf "    %-32s %s\n" "$$app" "$$first_brief"; \
		done; \
	done

# Forward `make <app>` to the per-app Makefile so the top-level shorthand
# and `cd examples/<tier>/<app> && make` produce the exact same artifacts.
# The per-app dir is looked up via RA_APP_DIR_<app> so callers don't
# need to know which tier directory the app lives in.
$(RA_APPS):
	$(MAKE) -C $(RA_APP_DIR_$@) build

clean:
	@for d in $(ROOT)/examples/*/*/main.c; do \
		[ -f "$$d" ] || continue; \
		$(MAKE) -C "$$(dirname $$d)" clean; \
	done
	rm -rf $(TESTS_BUILD) $(TESTS_BUILD_COV) $(TIDY_BUILD)

format:
	bash scripts/format_code.sh

check:
	bash scripts/format_code.sh --check

tidy:
	bash scripts/clang_tidy.sh --check

test-docker:
	bash scripts/test-docker.sh

test:
	bash $(TESTS_DIR)/build_tests.sh
	bash $(TESTS_DIR)/run_tests.sh

# `make test-cov` is an alias for `make mcdc` -- the coverage build
# tree (tests/build-cov/) lives in parallel with the fast tree
# (tests/build/) so toggling between them is incremental.
test-cov: mcdc

ctest:
	ctest --test-dir $(TESTS_BUILD) --output-on-failure

# `make fuzz` -- build every libFuzzer harness (clang only; opt-in via
# RA_FUZZ=ON) and run each for ~30 seconds as a smoke check. Crash
# artefacts land in tests/build-fuzz/crashes/<target>/. For longer
# fuzz sessions on a single target, use scripts/utils/run_fuzz.sh.
# See docs/FUZZING.md.
FUZZ_BUILD    := $(TESTS_DIR)/build-fuzz
FUZZ_SECONDS  ?= 30
FUZZ_TARGETS  := fuzz_ra_jpeg_sw fuzz_ra_jpeg_sw_block fuzz_ra_epub fuzz_ra_modem_at \
                 fuzz_ra_net_arp fuzz_ra_net_ipv4 fuzz_ra_ble_att fuzz_ra_usb_pal \
                 fuzz_ra_tls fuzz_ra_canfd fuzz_ra_etha fuzz_ra_fs_fat
FUZZ_CC       ?= clang
FUZZ_CXX      ?= clang++

fuzz:
	@command -v $(FUZZ_CC) >/dev/null 2>&1 || { \
	  echo "ERROR: $(FUZZ_CC) not found -- libFuzzer requires clang."; exit 1; }
	$(CMAKE) -S $(TESTS_DIR) -B $(FUZZ_BUILD) \
	    -DRA_FUZZ=ON -DRA_COVERAGE=OFF \
	    -DCMAKE_C_COMPILER=$(FUZZ_CC) \
	    -DCMAKE_CXX_COMPILER=$(FUZZ_CXX) \
	    -DCMAKE_BUILD_TYPE=Debug
	$(CMAKE) --build $(FUZZ_BUILD) --target ra_fuzz_all -j
	@bash scripts/utils/init_fuzz_corpora.sh
	@for t in $(FUZZ_TARGETS); do \
	  echo "==== Running $$t for $(FUZZ_SECONDS)s ===="; \
	  mkdir -p $(FUZZ_BUILD)/crashes/$$t; \
	  $(FUZZ_BUILD)/fuzz/$$t \
	      tests/fuzz/corpus/$$t \
	      -max_total_time=$(FUZZ_SECONDS) \
	      -runs=10000 \
	      -print_final_stats=1 \
	      -artifact_prefix=$(FUZZ_BUILD)/crashes/$$t/ \
	    || { echo "FUZZ FAIL: $$t"; exit 1; }; \
	done
	@echo "All fuzz smoke runs passed."

docs:
	bash scripts/build_docs.sh

dashboard:
	python3 scripts/utils/roadmap_dashboard.py

coverage:
	bash scripts/utils/coverage_report.sh
	python3 scripts/utils/check_coverage.py

mcdc:
	bash scripts/utils/mcdc_report.sh

# MISRA-C 2012 audit (advisory; see docs/MISRA.md). Outputs:
#   build/misra/results.txt  -- one TSV violation per line
#   build/misra/raw.txt      -- raw cppcheck stderr
#   build/misra/misra-raw.txt-- raw misra.py stdout
# Not gated by pre-commit yet.
misra:
	bash scripts/utils/misra_check.sh

ascii:
	@for dir in src libs tests; do \
		python3 scripts/utils/fix-encoding.py --check "$$dir" || exit 1; \
	done
	@for d in $(ROOT)/examples/*/*/main.c; do \
		[ -f "$$d" ] || continue; \
		python3 scripts/utils/fix-encoding.py --check "$$(dirname $$d)" || exit 1; \
	done

version:
	@echo "project VERSION: $$(cat VERSION)"
	@python3 scripts/utils/check-since-version.py --all

qe-test:
	python3 -m unittest tools/ra_qe/tests/test_generate.py

# Hardware smoke test -- builds every EVM-tier app, then flashes each
# one through the on-board J-Link OB and classifies the halt-PC as
# PASS / WIP / FAIL. See scripts/hw_smoke_test.sh for the rules.
# Exits non-zero if any app comes back FAIL (NOBUILD/WIP are warnings).
smoke: $(EK_APPS)
	bash scripts/hw_smoke_test.sh

# Stack-usage proof. Builds every EVM-tier app (each is already
# compiled with -fstack-usage via cmake/ra_warnings.cmake), then runs
# the python aggregator over the resulting .su files. Prints the
# worst-10 stack frames across all apps; exits non-zero if any
# critical-path module (ra_isr, ra_check, ra_err, ra_mpu, ra_cgc,
# ra_pfs) breaches its 256-byte ceiling or contains a `dynamic` frame.
# See docs/STACK_USAGE.md.
stack-usage: $(EK_APPS)
	python3 scripts/utils/stack_usage_check.py --top 10

# Clang Static Analyzer (scan-build). Drives a clean host-test build
# under tests/build-scan/ with the analyzer interposed; emits HTML
# reports under build/scan-build-reports/. See docs/STATIC_ANALYSIS.md
# for the documented baseline of expected findings.
scan-build:
	bash scripts/utils/scan_build.sh

# Strict variant -- exit non-zero on any first-party finding after the
# documented suppressions (HAL MMIO accessor pattern, SOUP, host test
# scaffolding) are applied. This is the CI-gating entry point.
scan-build-strict:
	bash scripts/utils/scan_build.sh --strict

# Annotation-attribute enforcement. Walks the AST of every TU via
# libclang (python3 -m pip install --user --break-system-packages
# libclang) and applies the 19 ra_* rules documented in
# docs/ANNOTATIONS.md. `make check-annotations` runs in CI mode
# (exits non-zero on violation); the bare hook invocation defaults
# to warn-only via the WAVE_0_WARN_ONLY flag at the top of the script.
check-annotations:
	python3 scripts/utils/check_annotations.py --check

# include-what-you-use (IWYU). Reports per-TU include-graph hygiene
# violations: missing direct includes, unnecessary transitive
# includes, and forward-decl opportunities. Warn-only today; see
# scripts/utils/iwyu.sh.
iwyu:
	bash scripts/utils/iwyu.sh

# ---------------------------------------------------------------------------
# `make bench` -- build + run the host-side performance microbenchmarks.
# Opt-in via -DRA_BENCH=ON; lives in its own build tree so the fast
# unit-test build (tests/build/) is never invalidated. Each bench
# binary prints a CSV results row per measurement to stdout.
# See docs/PERFORMANCE.md.
# ---------------------------------------------------------------------------
BENCH_BUILD   := $(TESTS_DIR)/build-bench
BENCH_TARGETS := bench_ra_crc bench_ra_jpeg_sw bench_ra_gfx_text

bench:
	$(CMAKE) -S $(TESTS_DIR) -B $(BENCH_BUILD) \
	    -DRA_BENCH=ON -DRA_COVERAGE=OFF -DRA_MCDC=OFF \
	    -DCMAKE_BUILD_TYPE=Release \
	    -Wno-dev
	$(CMAKE) --build $(BENCH_BUILD) --target ra_bench_all -j
	@echo ""
	@echo "==== ra_bench results ===="
	@for t in $(BENCH_TARGETS); do \
	    echo "---- $$t ----"; \
	    $(BENCH_BUILD)/bench/$$t || { echo "FAIL: $$t"; exit 1; }; \
	done
	@echo "==== ra_bench done ===="

# ---------------------------------------------------------------------------
# `make app-sizes` -- run scripts/utils/app_sizes.py to summarise per-app
# .text/.data/.bss footprints across every built example. If no apps
# have been built yet the script prints a hint and exits cleanly.
# Writes docs/APP_SIZES.md when --write is passed.
# ---------------------------------------------------------------------------
app-sizes:
	python3 scripts/utils/app_sizes.py --write

# ---------------------------------------------------------------------------
# `make audit-init` -- per-app init-order linter. Walks each main.c and
# verifies the canonical CGC -> MSTP -> IOPORT -> peripheral ordering
# is respected. Writes docs/INIT_ORDER_AUDIT.md as a side-effect.
# Warn-only today; pass --strict to fail on first violation.
# ---------------------------------------------------------------------------
.PHONY: audit-init
audit-init:
	python3 scripts/utils/audit_init_order.py --report docs/INIT_ORDER_AUDIT.md

all: format tidy test default
