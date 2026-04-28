#
# Top-level Makefile for ra8d2-firmware.
#
# Thin wrapper around cmake + the scripts/ helpers so everyday
# commands are short. All real work lives in CMakeLists.txt +
# tests/CMakeLists.txt + scripts/ -- this file does not run the
# compiler directly.
#
# Common targets:
#
#   make build     -- cross-compile the firmware (./build.sh)
#   make clean     -- remove build/ and tests/build/
#   make format    -- run clang-format --in-place on the whole tree
#   make check     -- run clang-format --dry-run (no changes)
#   make tidy      -- run clang-tidy against the whole tree
#   make test      -- build + run the host unit tests
#   make docs      -- run doxygen against Doxyfile.main
#   make flash     -- ./scripts/flash.sh against a J-Link
#   make debug     -- ./scripts/debug.sh attach via gdb
#   make size      -- dump arm-none-eabi-size output for the ELF
#   make ascii     -- scan for non-ASCII characters
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

SHELL := /bin/bash

ROOT         := $(abspath .)
BUILD_DIR    := $(ROOT)/build
TESTS_DIR    := $(ROOT)/tests
TESTS_BUILD  := $(ROOT)/build/tidy
ELF          := $(BUILD_DIR)/ra8d2-firmware.elf

CMAKE        ?= cmake
CLANG_FORMAT ?= clang-format
DOXYGEN      ?= doxygen
ARM_SIZE     ?= arm-none-eabi-size

.PHONY: help build clean format check tidy test test-docker ctest docs flash ozone debug size ascii version all examples

help:
	@echo "ra8d2-firmware make targets:"
	@echo "  build           cross-compile examples/blink (default) via ./build.sh"
	@echo "  example-<name>  cross-compile examples/<name>/main.c"
	@echo "                  e.g. 'make example-blink'"
	@echo "  examples        list every available example"
	@echo "  clean       remove build/ and tests/build/"
	@echo "  format      run clang-format in place"
	@echo "  check       run clang-format --dry-run"
	@echo "  tidy        run clang-tidy"
	@echo "  test        host-compile + run unit tests (Linux native)"
	@echo "  test-docker host-compile + run unit tests in Linux container"
	@echo "              (use this on macOS where MAP_FIXED is blocked)"
	@echo "  ctest       rerun just ctest (assumes already built)"
	@echo "  docs        run doxygen"
	@echo "  flash       scripts/flash.sh"
	@echo "  ozone       scripts/ozone.sh -- open SEGGER Ozone GUI debugger"
	@echo "  debug       scripts/debug.sh"
	@echo "  size        arm-none-eabi-size on the ELF"
	@echo "  ascii       fix-encoding.py --check"
	@echo "  version     verify @since tags match the VERSION file"

build:
	./build.sh

# `make example-<name>` -- build a specific example.
# The pattern target wipes any previous build dir so the EXAMPLE
# selector takes effect even if a different example was last built.
example-%:
	@if [ ! -f $(ROOT)/examples/$*/main.c ]; then \
		echo "error: examples/$*/main.c not found" >&2; \
		echo "" >&2; \
		echo "available examples:" >&2; \
		ls -1 $(ROOT)/examples 2>/dev/null | sed 's/^/  /' >&2; \
		exit 1; \
	fi
	rm -rf $(BUILD_DIR)
	EXAMPLE=$* ./build.sh

examples:
	@echo "Available examples:"
	@for d in $(ROOT)/examples/*/; do \
		name=$$(basename $$d); \
		first_brief=$$(grep -m1 "@brief" $$d/main.c 2>/dev/null | sed 's/.*@brief //'); \
		printf "  %-15s %s\n" "$$name" "$$first_brief"; \
	done

clean:
	rm -rf $(BUILD_DIR) $(TESTS_BUILD)

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
	$(DOXYGEN) Doxyfile.main

flash:
	bash scripts/flash.sh

ozone:
	bash scripts/ozone.sh

debug:
	bash scripts/debug.sh

size:
	@test -f $(ELF) || { echo "No $(ELF) -- run 'make build' first"; exit 1; }
	$(ARM_SIZE) -A -x $(ELF)
	$(ARM_SIZE) -B $(ELF)

ascii:
	python3 scripts/utils/fix-encoding.py --check src libs tests examples

version:
	@echo "project VERSION: $$(cat VERSION)"
	@python3 scripts/utils/check-since-version.py --all

all: format tidy test build
