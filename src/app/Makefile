# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Makefile for the main e-reader application (src/app).
#
# Standalone use:
#   cd src/app/
#   make             # configure + build -> src/app/build/ra8d2-ereader.elf
#   make flash       # JLinkExe load via scripts/dev/flash.sh
#   make clean       # rm -rf src/app/build
#

SHELL     := /bin/bash

APP        := ra8d2-ereader
ROOT       := $(shell git -C "$(CURDIR)" rev-parse --show-toplevel)
HERE       := $(CURDIR)
BUILD_DIR  := $(HERE)/build
ELF        := $(BUILD_DIR)/$(APP).elf
HEX        := $(BUILD_DIR)/$(APP).hex
TOOLCHAIN  := $(ROOT)/cmake/toolchain-ra8d2.cmake

CMAKE      ?= cmake
ARM_SIZE   ?= arm-none-eabi-size

.PHONY: all build clean flash size help

all: build

help:
	@echo "$(APP) make targets:"
	@echo "  build / all  cross-compile $(APP).elf / .hex (default)"
	@echo "  clean        rm -rf $(BUILD_DIR)"
	@echo "  flash        scripts/dev/flash.sh $(HEX)"
	@echo "  size         arm-none-eabi-size on the ELF"

build: $(ELF)

$(ELF): $(wildcard $(HERE)/*.c) $(wildcard $(HERE)/*.h) $(HERE)/linker_script.ld $(HERE)/CMakeLists.txt
	$(CMAKE) -S $(HERE) -B $(BUILD_DIR) \
	    -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) \
	    -DCMAKE_BUILD_TYPE=$(or $(BUILD_TYPE),RelWithDebInfo) \
	    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
	$(CMAKE) --build $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)

flash:
	@test -f $(HEX) || { echo "No $(HEX) -- run 'make build' first"; exit 1; }
	bash $(ROOT)/scripts/dev/flash.sh $(HEX)

size:
	@test -f $(ELF) || { echo "No $(ELF) -- run 'make build' first"; exit 1; }
	$(ARM_SIZE) -A -x $(ELF)
	$(ARM_SIZE) -B $(ELF)
