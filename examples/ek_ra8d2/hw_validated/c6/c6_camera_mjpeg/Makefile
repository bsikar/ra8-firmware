# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

SHELL := /bin/bash
APP := c6_camera_mjpeg
ROOT := $(shell git -C "$(CURDIR)" rev-parse --show-toplevel)
HERE := $(CURDIR)
BUILD_DIR ?= $(HERE)/build
TOOLCHAIN := $(ROOT)/cmake/toolchain-ra8d2.cmake

.PHONY: all build clean flash
all: build
build:
	$(CMAKE) -S $(HERE) -B $(BUILD_DIR) \
	  -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) -DCMAKE_BUILD_TYPE=RelWithDebInfo \
	  -DRA8_USE_THREADX=ON -DRA8_USE_NETXDUO=ON -DRA8_USE_ESP_HOSTED=ON
	$(CMAKE) --build $(BUILD_DIR)
clean:
	rm -rf $(BUILD_DIR)
flash: build
	bash $(ROOT)/scripts/dev/flash.sh $(BUILD_DIR)/$(APP).hex

CMAKE ?= cmake
