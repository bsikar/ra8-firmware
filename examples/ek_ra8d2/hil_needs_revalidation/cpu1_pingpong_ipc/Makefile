#
# Per-app Makefile for the `cpu1_pingpong_ipc` firmware.
#
# Standalone use:
#   cd examples/ek_ra8d2/hw_pending/cpu1_pingpong_ipc/
#   make             # configure + build -> build/cpu1_pingpong_ipc.elf
#   make clean       # rm -rf build
#
# Builds with RA8_TRUSTZONE_ENABLE=ON by default -- the IPCSAR write
# this app needs is Secure-only, so the secure-boot path is mandatory.
# This app is hw_pending: bench validation is NOT performed by the
# host; flashing is left to a human operator who can recover via
# scripts/hil/recover.sh if the SAU layout turns out to brick the
# chip (see memory/project_sau_sgstubs_brick.md).
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

SHELL     := /bin/bash

APP        := cpu1_pingpong_ipc
ROOT       := $(shell git -C "$(CURDIR)" rev-parse --show-toplevel)
HERE       := $(CURDIR)
BUILD_DIR  := $(HERE)/build
ELF        := $(BUILD_DIR)/$(APP).elf
HEX        := $(BUILD_DIR)/$(APP).hex
BIN        := $(BUILD_DIR)/$(APP).bin
TOOLCHAIN  := $(ROOT)/cmake/toolchain-ra8d2.cmake

CMAKE      ?= cmake
ARM_SIZE   ?= arm-none-eabi-size

.PHONY: all build clean flash ozone debug size help

all: build

help:
	@echo "$(APP) make targets:"
	@echo "  build / all  cross-compile $(APP).elf / .hex / .bin (default)"
	@echo "  clean        rm -rf $(BUILD_DIR)"
	@echo "  flash        scripts/dev/flash.sh $(HEX)  (BENCH ONLY -- brick risk)"
	@echo "  ozone        scripts/dev/ozone.sh $(ELF)"
	@echo "  debug        scripts/dev/debug.sh $(ELF)"
	@echo "  size         arm-none-eabi-size on the ELF"

build: $(ELF)

$(ELF): $(wildcard $(HERE)/*.c) $(wildcard $(HERE)/*.h) $(wildcard $(HERE)/linker_script.ld) $(HERE)/CMakeLists.txt
	$(CMAKE) -S $(HERE) -B $(BUILD_DIR) \
	    -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) \
	    -DCMAKE_BUILD_TYPE=$(or $(BUILD_TYPE),RelWithDebInfo) \
	    -DRA8_TRUSTZONE_ENABLE=ON \
	    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
	$(CMAKE) --build $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)

flash:
	@test -f $(HEX) || { echo "No $(HEX) -- run 'make build' first"; exit 1; }
	bash $(ROOT)/scripts/dev/flash.sh $(HEX)

ozone:
	@test -f $(ELF) || { echo "No $(ELF) -- run 'make build' first"; exit 1; }
	bash $(ROOT)/scripts/dev/ozone.sh $(ELF)

debug:
	@test -f $(ELF) || { echo "No $(ELF) -- run 'make build' first"; exit 1; }
	bash $(ROOT)/scripts/dev/debug.sh $(ELF)

size:
	@test -f $(ELF) || { echo "No $(ELF) -- run 'make build' first"; exit 1; }
	$(ARM_SIZE) -A -x $(ELF)
	$(ARM_SIZE) -B $(ELF)
