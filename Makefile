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
.PHONY: help apps default clean format check tidy test test-cov test-docker ctest coverage mcdc misra docs dashboard ascii version qe-test smoke stack-usage scan-build all $(RA_APPS)

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

docs:
	bash scripts/build_docs.sh

dashboard:
	python3 scripts/utils/roadmap_dashboard.py

coverage:
	bash scripts/coverage_report.sh

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

all: format tidy test default
