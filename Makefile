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

.PHONY: help build clean format check tidy test ctest docs flash debug size ascii all

help:
	@echo "ra8d2-firmware make targets:"
	@echo "  build    cross-compile via ./build.sh"
	@echo "  clean    remove build/ and tests/build/"
	@echo "  format   run clang-format in place"
	@echo "  check    run clang-format --dry-run"
	@echo "  tidy     run clang-tidy"
	@echo "  test     host-compile + run unit tests"
	@echo "  ctest    rerun just ctest (assumes already built)"
	@echo "  docs     run doxygen"
	@echo "  flash    scripts/flash.sh"
	@echo "  debug    scripts/debug.sh"
	@echo "  size     arm-none-eabi-size on the ELF"
	@echo "  ascii    fix-encoding.py --check"

build:
	./build.sh

clean:
	rm -rf $(BUILD_DIR) $(TESTS_BUILD)

format:
	bash scripts/format_code.sh

check:
	bash scripts/format_code.sh --check

tidy:
	bash scripts/clang_tidy.sh --check

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

debug:
	bash scripts/debug.sh

size:
	@test -f $(ELF) || { echo "No $(ELF) -- run 'make build' first"; exit 1; }
	$(ARM_SIZE) -A -x $(ELF)
	$(ARM_SIZE) -B $(ELF)

ascii:
	python3 scripts/utils/fix-encoding.py --check src libs tests

all: format tidy test build
