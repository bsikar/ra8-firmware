# Host floor-plan demo -- thin wrapper over the CMake build.
#
# make        configure + build
# make run    build then launch the window
# make clean  remove the build tree
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT

BUILD := build
# Render into the (gitignored) build dir so the image never lands in the tree.
PNG   := $(BUILD)/floorplan.png

.PHONY: all run png clean

all:
	cmake -B $(BUILD) -S .
	cmake --build $(BUILD)

run: all
	$(BUILD)/floorplan

# Headless render to a PNG (no window) -- handy for review / CI screenshots.
png: all
	$(BUILD)/floorplan --png $(PNG)
	@echo "wrote $(CURDIR)/$(PNG)"

clean:
	rm -rf $(BUILD)
