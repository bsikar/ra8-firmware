# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

SHELL := /bin/bash

APP       := media_download
ROOT      := $(shell git -C "$(CURDIR)" rev-parse --show-toplevel)
HERE      := $(CURDIR)
BUILD_DIR := $(HERE)/build
ELF       := $(BUILD_DIR)/$(APP).elf
HEX       := $(BUILD_DIR)/$(APP).hex
PROOF_BUILD_DIR := $(HERE)/build-proof
PROOF_ELF       := $(PROOF_BUILD_DIR)/$(APP).elf
TOOLCHAIN := $(ROOT)/cmake/toolchain-ra8d2.cmake
WIFI_ENV  := $(ROOT)/coprocessor/esp32c6/wifi.env

CMAKE    ?= cmake
ARM_NM   ?= arm-none-eabi-nm
ARM_SIZE ?= arm-none-eabi-size

.PHONY: all build clean compile-proof flash ozone debug size help

all: build

help:
	@echo "$(APP): C6 HTTPS body -> strict transactional SD .rabook"
	@echo "  build  cross-compile with optional URL, Wi-Fi, and SOURCE_IMAGE env"
	@echo "  compile-proof  retain and assert the complete configured transfer path"
	@echo "  clean  remove $(BUILD_DIR) and $(PROOF_BUILD_DIR)"
	@echo "  flash / ozone / debug / size  standard board workflows"

build:
	@if [ -f "$(WIFI_ENV)" ]; then set -a; . "$(WIFI_ENV)"; set +a; fi; \
	$(CMAKE) -S $(HERE) -B $(BUILD_DIR) \
	    -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) \
	    -DCMAKE_BUILD_TYPE=$(or $(BUILD_TYPE),RelWithDebInfo) \
	    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
	    -DRA8_USE_THREADX=ON \
	    -DRA8_USE_ESP_HOSTED=ON \
	    $${RA8_MEDIA_DOWNLOAD_URL:+-DRA8_MEDIA_DOWNLOAD_URL="$$RA8_MEDIA_DOWNLOAD_URL"} \
	    $${RA8_MEDIA_DOWNLOAD_SOURCE_IMAGE:+-DRA8_MEDIA_DOWNLOAD_SOURCE_IMAGE="$$RA8_MEDIA_DOWNLOAD_SOURCE_IMAGE"} \
	    $${RA8_C6_WIFI_SSID:+-DRA8_C6_WIFI_SSID="$$RA8_C6_WIFI_SSID"} \
	    $${RA8_C6_WIFI_PSK:+-DRA8_C6_WIFI_PSK="$$RA8_C6_WIFI_PSK"} && \
	$(CMAKE) --build $(BUILD_DIR)

compile-proof:
	$(CMAKE) -S $(HERE) -B $(PROOF_BUILD_DIR) \
	    -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) \
	    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
	    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
	    -DRA8_USE_THREADX=ON \
	    -DRA8_USE_ESP_HOSTED=ON \
	    -DRA8_MEDIA_DOWNLOAD_URL=https://compile-proof.invalid/book.rabook \
	    -DRA8_C6_WIFI_SSID=compile-proof \
	    -DRA8_C6_WIFI_PSK=compile-proof-pass
	$(CMAKE) --build $(PROOF_BUILD_DIR)
	@$(ARM_NM) --defined-only $(PROOF_ELF) | \
	    grep -Eq '[[:space:]]ra8_c6link_mdl_transfer$$'
	@$(ARM_NM) --defined-only $(PROOF_ELF) | \
	    grep -Eq '[[:space:]]ra8_mdl_rabook_vfs_validate$$'
	@$(ARM_SIZE) -A $(PROOF_ELF) | \
	    awk '$$1 == ".sdram_data" && $$2 > 0 { found = 1 } END { exit !found }'

clean:
	$(CMAKE) -E remove_directory $(BUILD_DIR)
	$(CMAKE) -E remove_directory $(PROOF_BUILD_DIR)

flash:
	@test -f $(HEX) || { echo "No $(HEX) -- run make first"; exit 1; }
	bash $(ROOT)/scripts/dev/flash.sh $(HEX)

ozone:
	@test -f $(ELF) || { echo "No $(ELF) -- run make first"; exit 1; }
	bash $(ROOT)/scripts/dev/ozone.sh $(ELF)

debug:
	@test -f $(ELF) || { echo "No $(ELF) -- run make first"; exit 1; }
	bash $(ROOT)/scripts/dev/debug.sh $(ELF)

size:
	@test -f $(ELF) || { echo "No $(ELF) -- run make first"; exit 1; }
	$(ARM_SIZE) -B $(ELF)
