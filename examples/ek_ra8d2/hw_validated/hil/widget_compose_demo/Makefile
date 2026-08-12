# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app Makefile for the `widget_compose_demo` firmware.
#
# Standalone use:
#   cd examples/ek_ra8d2/hw_pending/widget_compose_demo/
#   make             # configure + build -> build/widget_compose_demo.elf
#   make flash       # JLinkExe load via scripts/dev/flash.sh
#   make ozone       # SEGGER Ozone GUI debugger
#   make clean       # rm -rf ./build
#
#

SHELL     := /bin/bash

APP        := widget_compose_demo
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
	@echo "  flash        scripts/dev/flash.sh $(HEX)"
	@echo "  ozone        scripts/dev/ozone.sh $(ELF)"
	@echo "  debug        scripts/dev/debug.sh $(ELF)"
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
