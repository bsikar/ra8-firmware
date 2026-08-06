# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Makefile for module_hello.
#
# Standalone use:
#   cd examples/ek_ra8d2/hw_pending/module_hello/
#   make             # configure + build -> build/module_hello.elf
#   make flash       # JLinkExe load via scripts/dev/flash.sh
#   make clean       # rm -rf build
#

SHELL     := /bin/bash

APP        := module_hello
ROOT       := $(shell git -C "$(CURDIR)" rev-parse --show-toplevel)
HERE       := $(CURDIR)
BUILD_DIR  := $(HERE)/build
ELF        := $(BUILD_DIR)/$(APP).elf
HEX        := $(BUILD_DIR)/$(APP).hex

TOOLCHAIN  := $(ROOT)/cmake/toolchain-ra8d2.cmake
CMAKE      ?= cmake

.PHONY: all flash clean

all: $(ELF)

$(ELF): CMakeLists.txt main.c $(wildcard $(ROOT)/cmake/*.cmake)
	$(CMAKE) -S $(HERE) -B $(BUILD_DIR) \
	    -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) \
	    -DCMAKE_BUILD_TYPE=$(or $(BUILD_TYPE),RelWithDebInfo) \
	    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
	$(CMAKE) --build $(BUILD_DIR)

flash: $(ELF)
	$(ROOT)/scripts/dev/flash.sh $(ELF)

clean:
	rm -rf $(BUILD_DIR)
