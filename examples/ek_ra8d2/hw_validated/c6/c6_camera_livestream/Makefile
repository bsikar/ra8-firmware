# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

SHELL := /bin/bash
APP := c6_camera_livestream
ROOT := $(shell git -C "$(CURDIR)" rev-parse --show-toplevel)
HERE := $(CURDIR)
BUILD_DIR := $(HERE)/build
ELF := $(BUILD_DIR)/$(APP).elf
HEX := $(BUILD_DIR)/$(APP).hex
WIFI_ENV := $(ROOT)/coprocessor/esp32c6/wifi.env
TOOLCHAIN := $(ROOT)/cmake/toolchain-ra8d2.cmake
CAMERA_DIR := $(ROOT)/examples/ek_ra8d2/hw_validated/hil/camera_capture
APP_SOURCES := $(wildcard $(HERE)/*.c) $(wildcard $(HERE)/src/*.c)
APP_HEADERS := $(wildcard $(HERE)/*.h) $(wildcard $(HERE)/*.h.in)
APP_TEMPLATES := $(wildcard $(HERE)/*.in)
CAMERA_SOURCES := $(CAMERA_DIR)/src/cam_ceu.c $(CAMERA_DIR)/src/cam_ov5640.c
CAMERA_HEADERS := $(CAMERA_DIR)/cam_ceu.h $(CAMERA_DIR)/cam_ov5640.h
CMAKE ?= cmake

.PHONY: all build clean flash
all: build
build: $(ELF)

$(ELF): $(APP_SOURCES) $(APP_HEADERS) $(APP_TEMPLATES) $(CAMERA_SOURCES) \
	$(CAMERA_HEADERS) $(HERE)/CMakeLists.txt $(HERE)/Makefile
	@if [ -f "$(WIFI_ENV)" ]; then set -a; . "$(WIFI_ENV)"; set +a; fi; \
	$(CMAKE) -S $(HERE) -B $(BUILD_DIR) \
	  -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) -DCMAKE_BUILD_TYPE=RelWithDebInfo \
	  -DRA8_USE_THREADX=ON -DRA8_USE_NETXDUO=ON -DRA8_USE_ESP_HOSTED=ON && \
	$(CMAKE) --build $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)

flash: $(ELF)
	bash $(ROOT)/scripts/dev/flash.sh $(HEX)
