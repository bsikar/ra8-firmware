# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app Makefile for the `c6_wifi_join` firmware.
#
# Standalone:
#   cd examples/ek_ra8d2/hw_validated/c6/c6_wifi_join/
#   make             # configure + build (auto-enables ThreadX + NetX Duo + esp-hosted)
#   make flash       # JLinkExe load via scripts/dev/flash.sh
#   make clean
#
# Wi-Fi credentials are NOT in the tree. They are read from the environment at
# BUILD time and compiled in:
#
#   RA8_C6_WIFI_SSID=ra8-bench \
#   RA8_C6_WIFI_PSK="$(python3 ../../../../../scripts/secrets/openbao_client.py ... )" \
#   make
#
# or drop them in the gitignored coprocessor/esp32c6/wifi.env (see
# wifi.env.example), which this Makefile sources automatically. With neither
# set the image still builds -- it prints a FAIL at runtime instead of baking a
# blank credential -- so the aggregate cross-build stays green.
#
#

SHELL     := /bin/bash

APP        := c6_wifi_join
ROOT       := $(shell git -C "$(CURDIR)" rev-parse --show-toplevel)
HERE       := $(CURDIR)
BUILD_DIR  := $(HERE)/build
ELF        := $(BUILD_DIR)/$(APP).elf
HEX        := $(BUILD_DIR)/$(APP).hex
BIN        := $(BUILD_DIR)/$(APP).bin
TOOLCHAIN  := $(ROOT)/cmake/toolchain-ra8d2.cmake
WIFI_ENV   := $(ROOT)/coprocessor/esp32c6/wifi.env

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
	@echo ""
	@echo "  Wi-Fi creds: set RA8_C6_WIFI_SSID / RA8_C6_WIFI_PSK in the env or"
	@echo "  in $(WIFI_ENV) (gitignored). Empty is allowed; it just FAILs at run."

build: $(ELF)

$(ELF): $(wildcard $(HERE)/*.c) $(wildcard $(HERE)/*.h) $(wildcard $(HERE)/src/*.c) \
       $(wildcard $(HERE)/linker_script.ld) $(HERE)/CMakeLists.txt
	@if [ -f "$(WIFI_ENV)" ]; then set -a; . "$(WIFI_ENV)"; set +a; fi; \
	$(CMAKE) -S $(HERE) -B $(BUILD_DIR) \
	    -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) \
	    -DCMAKE_BUILD_TYPE=$(or $(BUILD_TYPE),RelWithDebInfo) \
	    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
	    -DRA8_USE_THREADX=ON \
	    -DRA8_USE_NETXDUO=ON \
	    -DRA8_USE_ESP_HOSTED=ON && \
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
