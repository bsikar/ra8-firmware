# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app Makefile for the RA8P1 build-foundation `blink_ra8p1`.
#
# Standalone use:
#   cd examples/ra8p1_foundation/blink_ra8p1/
#   make             # configure + build -> ./build/blink_ra8p1.elf (RA8P1)
#   make size        # arm-none-eabi-size on the ELF
#   make clean       # rm -rf ./build
#
# The ONLY difference from a stock RA8D2 app Makefile is the toolchain file:
# cmake/toolchain-ra8p1.cmake selects the RA8P1 (adds -DRA8_DEVICE_RA8P1). The
# CPU flags, compiler, and linker flags are shared with the RA8D2 toolchain.
#
#

SHELL     := /bin/bash

APP        := blink_ra8p1
ROOT       := $(shell git -C "$(CURDIR)" rev-parse --show-toplevel)
HERE       := $(CURDIR)
BUILD_DIR  := $(HERE)/build
ELF        := $(BUILD_DIR)/$(APP).elf
HEX        := $(BUILD_DIR)/$(APP).hex
BIN        := $(BUILD_DIR)/$(APP).bin
TOOLCHAIN  := $(ROOT)/cmake/toolchain-ra8p1.cmake

CMAKE      ?= cmake
ARM_SIZE   ?= arm-none-eabi-size

.PHONY: all build clean size help

all: build

help:
	@echo "$(APP) make targets (RA8P1 foundation):"
	@echo "  build / all  cross-compile $(APP).elf / .hex / .bin for R7KA8P1KFLCAC (default)"
	@echo "  clean        rm -rf $(BUILD_DIR)"
	@echo "  size         arm-none-eabi-size on the ELF"

build: $(ELF)

$(ELF): $(wildcard $(HERE)/*.c) $(wildcard $(HERE)/*.h) $(wildcard $(HERE)/linker_script.ld) $(HERE)/CMakeLists.txt
	$(CMAKE) -S $(HERE) -B $(BUILD_DIR) \
	    -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) \
	    -DCMAKE_BUILD_TYPE=$(or $(BUILD_TYPE),RelWithDebInfo) \
	    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
	$(CMAKE) --build $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)

size:
	@test -f $(ELF) || { echo "No $(ELF) -- run 'make build' first"; exit 1; }
	$(ARM_SIZE) -A -x $(ELF)
	$(ARM_SIZE) -B $(ELF)
