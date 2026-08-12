# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app Makefile for the `wifi_hal_join` firmware -- the HAL-way twin of
# c6_wifi_join, driving the same join+DHCP through the ra8_wifi facade.
#
# Standalone:
#   cd examples/ek_ra8d2/hw_validated/c6/wifi_hal_join/
#   make             # configure + build (auto-enables ThreadX + NetX Duo + esp-hosted)
#   make flash       # JLinkExe load via scripts/dev/flash.sh
#   make clean
#
# Wi-Fi credentials are NOT in the tree. They are read from the environment at
# BUILD time, from the gitignored coprocessor/esp32c6/wifi.env, or auto-fetched
# from OpenBao (secret/ra8d2/bench-network). With none available, the image builds
# safely with empty credentials and reports a FAIL at runtime.
#
#

SHELL     := /bin/bash

APP        := wifi_hal_join
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
	@echo "  Wi-Fi creds: set RA8_C6_WIFI_SSID / RA8_C6_WIFI_PSK in env, in"
	@echo "  $(WIFI_ENV), or auto-fetch from OpenBao (secret/ra8d2/bench-network)."

build: $(ELF)

$(ELF): $(wildcard $(HERE)/*.c) $(wildcard $(HERE)/*.h) $(wildcard $(HERE)/src/*.c) \
       $(wildcard $(HERE)/linker_script.ld) $(HERE)/CMakeLists.txt
	@if [ -f "$(WIFI_ENV)" ]; then set -a; . "$(WIFI_ENV)"; set +a; fi; \
	if [ -z "$$RA8_C6_WIFI_PSK" ] && [ -f "$(ROOT)/scripts/secrets/openbao_client.py" ]; then \
		_bao_psk=$$(python3 "$(ROOT)/scripts/secrets/openbao_client.py" get secret/ra8d2/bench-network bench_psk 2>/dev/null || true); \
		_bao_ssid=$$(python3 "$(ROOT)/scripts/secrets/openbao_client.py" get secret/ra8d2/bench-network bench_ssid 2>/dev/null || true); \
		if [ -n "$$_bao_psk" ]; then \
			export RA8_C6_WIFI_SSID="$${_bao_ssid:-ra8-bench}"; \
			export RA8_C6_WIFI_PSK="$$_bao_psk"; \
			echo "[Wi-Fi] Auto-sourced credentials (SSID='$$RA8_C6_WIFI_SSID') from OpenBao (secret/ra8d2/bench-network)"; \
		fi; \
	fi; \
	$(CMAKE) -S $(HERE) -B $(BUILD_DIR) \
	    -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) \
	    -DCMAKE_BUILD_TYPE=$(or $(BUILD_TYPE),RelWithDebInfo) \
	    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
	    -DRA8_USE_THREADX=ON \
	    -DRA8_USE_NETXDUO=ON \
	    -DRA8_USE_ESP_HOSTED=ON \
	    $${RA8_C6_WIFI_SSID:+-DRA8_C6_WIFI_SSID="$$RA8_C6_WIFI_SSID"} \
	    $${RA8_C6_WIFI_PSK:+-DRA8_C6_WIFI_PSK="$$RA8_C6_WIFI_PSK"} && \
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
