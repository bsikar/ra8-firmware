# Host GUIX 3-tab bedroom UI demo -- thin wrapper over the CMake build.
#
# make        configure + build
# make run    build then launch the window (click the tabs)
# make png    build then render a tab headless to a PNG (via sips)
# make clean  remove the build tree
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT

BUILD := build
# Render into the (gitignored) build dir so images never land in the tree.
PPM   := $(BUILD)/guix_tabs.ppm
PNG   := $(BUILD)/guix_tabs.png
# Which tab to render headless (0=Primary, 1=Secondary 1, 2=Secondary 2).
TAB   ?= 0

.PHONY: all run png clean

all:
	cmake -B $(BUILD) -S .
	cmake --build $(BUILD)

run: all
	$(BUILD)/guix_tabs

# Headless render to a PNG (no window) -- handy for review / screenshots.
# guix_tabs writes PPM (no image-lib dependency); sips converts to PNG.
png: all
	$(BUILD)/guix_tabs --png $(PPM) --tab $(TAB)
	sips -s format png $(PPM) --out $(PNG) >/dev/null
	@echo "wrote $(CURDIR)/$(PNG)"

clean:
	rm -rf $(BUILD)
