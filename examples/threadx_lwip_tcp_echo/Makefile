#
# Per-app Makefile for the `threadx_lwip_tcp_echo` firmware.
#
# Standalone:
#   cd examples/threadx_lwip_tcp_echo/
#   make             # configure + build (auto-enables ThreadX + lwIP)
#   make flash       # JLinkExe load via scripts/flash.sh
#   make clean
#
# This wraps the per-app CMakeLists.txt with -DRA_USE_THREADX=ON
# -DRA_USE_LWIP=ON forced on, since the demo cannot build without
# them.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

SHELL     := /bin/bash

APP        := threadx_lwip_tcp_echo
ROOT       := $(abspath $(CURDIR)/../..)
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
	@echo "  flash        scripts/flash.sh $(HEX)"
	@echo "  ozone        scripts/ozone.sh $(ELF)"
	@echo "  debug        scripts/debug.sh $(ELF)"
	@echo "  size         arm-none-eabi-size on the ELF"

build: $(ELF)

$(ELF): $(wildcard $(HERE)/*.c) $(wildcard $(HERE)/*.h) $(HERE)/linker_script.ld $(HERE)/CMakeLists.txt
	$(CMAKE) -S $(HERE) -B $(BUILD_DIR) \
	    -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) \
	    -DCMAKE_BUILD_TYPE=Debug \
	    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
	    -DRA_USE_THREADX=ON \
	    -DRA_USE_LWIP=ON
	$(CMAKE) --build $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)

flash:
	@test -f $(HEX) || { echo "No $(HEX) -- run 'make build' first"; exit 1; }
	bash $(ROOT)/scripts/flash.sh $(HEX)

ozone:
	@test -f $(ELF) || { echo "No $(ELF) -- run 'make build' first"; exit 1; }
	bash $(ROOT)/scripts/ozone.sh $(ELF)

debug:
	@test -f $(ELF) || { echo "No $(ELF) -- run 'make build' first"; exit 1; }
	bash $(ROOT)/scripts/debug.sh $(ELF)

size:
	@test -f $(ELF) || { echo "No $(ELF) -- run 'make build' first"; exit 1; }
	$(ARM_SIZE) -A -x $(ELF)
	$(ARM_SIZE) -B $(ELF)
