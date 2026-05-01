#
# Top-level Makefile for ra8d2-firmware.
#
# Each application lives in its own top-level directory with its own
# main.c, boot files (vector_table.c, system_init.c,
# secure_exception.c, trustzone_init.c), linker_script.ld, Makefile,
# and CMakeLists.txt. This top-level Makefile auto-discovers them
# (any top-level dir containing a main.c is an app) and forwards
# `make <app>` to that app's per-app Makefile.
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
#   make ascii      -- scan for non-ASCII characters
#   make version    -- verify @since tags match the VERSION file
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

SHELL := /bin/bash

ROOT         := $(abspath .)
TESTS_DIR    := $(ROOT)/tests
TESTS_BUILD  := $(ROOT)/build/tidy

CMAKE        ?= cmake
CLANG_FORMAT ?= clang-format
DOXYGEN      ?= doxygen
ARM_SIZE     ?= arm-none-eabi-size

# Default app -- override on the command line, e.g. `make RA_DEFAULT_APP=blink_hal`.
RA_DEFAULT_APP ?= blink

# Auto-discover apps: every examples/<name>/ dir with main.c + CMakeLists.txt.
# RA_APPS holds the bare names (e.g. "blink"); the per-app dir lives at
# $(ROOT)/examples/$(app).
RA_APPS := $(sort $(patsubst $(ROOT)/examples/%/main.c,%,$(wildcard $(ROOT)/examples/*/main.c)))

# We forward each <app> name to the app's own Makefile, so reserve the names
# below from being shadowed by .PHONY targets.
.PHONY: help apps default clean format check tidy test test-docker ctest coverage docs ascii version qe-test all $(RA_APPS)

help:
	@echo "ra8d2-firmware make targets:"
	@echo "  make           build the default app ($(RA_DEFAULT_APP))"
	@echo "  make <app>     build a specific app -- one of: $(RA_APPS)"
	@echo "  make apps      list every discovered app"
	@echo "  make clean     remove every app build dir and tests/build"
	@echo "  make format    run clang-format in place"
	@echo "  make check     run clang-format --dry-run"
	@echo "  make tidy      run clang-tidy"
	@echo "  make test      host-compile + run unit tests (Linux native)"
	@echo "  make test-docker host-compile + run unit tests in Linux container"
	@echo "  make ctest     rerun just ctest (assumes already built)"
	@echo "  make coverage  generate lcov+genhtml HTML coverage report"
	@echo "  make docs      generate doxygen HTML into build/docs/html/"
	@echo "  make ascii     fix-encoding.py --check"
	@echo "  make version   verify @since tags match the VERSION file"
	@echo "  make qe-test   run tools/ra_qe (JSON configurator) unit tests"

# `make` with no arg builds the default app.
default: $(RA_DEFAULT_APP)

apps:
	@echo "Available apps:"
	@for app in $(RA_APPS); do \
		first_brief=$$(grep -m1 "@brief" $(ROOT)/examples/$$app/main.c 2>/dev/null | sed 's/.*@brief //'); \
		printf "  %-15s %s\n" "$$app" "$$first_brief"; \
	done

# Forward `make <app>` to the per-app Makefile so the top-level shorthand
# and `cd examples/<app> && make` produce the exact same artifacts.
$(RA_APPS):
	$(MAKE) -C $(ROOT)/examples/$@ build

clean:
	@for app in $(RA_APPS); do \
		$(MAKE) -C $(ROOT)/examples/$$app clean; \
	done
	rm -rf $(TESTS_BUILD)

format:
	bash scripts/format_code.sh

check:
	bash scripts/format_code.sh --check

tidy:
	bash scripts/clang_tidy.sh --check

test-docker:
	bash scripts/test-docker.sh

test:
	$(CMAKE) -B $(TESTS_BUILD) -S $(TESTS_DIR) \
		-DCMAKE_EXPORT_COMPILE_COMMANDS=ON -Wno-dev
	$(CMAKE) --build $(TESTS_BUILD) --parallel
	ctest --test-dir $(TESTS_BUILD) --output-on-failure

ctest:
	ctest --test-dir $(TESTS_BUILD) --output-on-failure

docs:
	bash scripts/build_docs.sh

coverage:
	bash scripts/coverage_report.sh

ascii:
	@for dir in src libs tests $(RA_APPS); do \
		python3 scripts/utils/fix-encoding.py --check "$$dir" || exit 1; \
	done

version:
	@echo "project VERSION: $$(cat VERSION)"
	@python3 scripts/utils/check-since-version.py --all

qe-test:
	python3 -m unittest tools/ra_qe/tests/test_generate.py

all: format tidy test default
