# Makefile -- our-own ESP32-C6 build (no idf.py, no ESP-IDF).
#
# Cross-compiles a minimal RAM-app firmware with the standard low-level RISC-V
# toolchain (riscv64-elf-gcc, rv32imac/ilp32) + our linker script + our start.S,
# then objcopy to a flat .bin. The `image`, `flash`, and `sim` targets are
# roadmap stubs (they point at the tool that will do the work).
#
# Usage:
#   make            -> build/esp32c6-blink.elf + .bin
#   make image      -> (roadmap) esp_mkimage.py packs the Espressif app image
#   make flash      -> (roadmap) esp_flash.py downloads to a connected C6
#   make sim        -> (roadmap) host-side simulator run
#   make clean
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT

CROSS   := riscv64-elf-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump

BUILD   := build
APP     := esp32c6-blink

# Architecture: single HP RISC-V core, rv32imac / ilp32, little-endian.
ARCHFLAGS := -march=rv32imac -mabi=ilp32

CFLAGS := $(ARCHFLAGS) -std=c23 -nostdlib -ffreestanding -Os \
          -Wall -Wextra -Werror \
          -ffunction-sections -fdata-sections \
          -Ihal -Idrivers/ours

ASFLAGS := $(ARCHFLAGS)

LDFLAGS := $(ARCHFLAGS) -nostdlib -Wl,--gc-sections -Wl,-Tboot/esp32c6.ld

C_SRCS := src/main.c drivers/ours/esp_gpio.c drivers/ours/esp_uart.c
S_SRCS := boot/start.S
OBJS   := $(addprefix $(BUILD)/,$(C_SRCS:.c=.o) $(S_SRCS:.S=.o))

ELF := $(BUILD)/$(APP).elf
BIN := $(BUILD)/$(APP).bin

.PHONY: all clean image flash sim

all: $(ELF) $(BIN)
	@echo "built $(ELF) + $(BIN)"

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

$(ELF): $(OBJS) boot/esp32c6.ld
	$(CC) $(LDFLAGS) $(OBJS) -o $@

$(BIN): $(ELF)
	$(OBJCOPY) -O binary $< $@

image: $(BIN)
	@echo "roadmap: Espressif image packing is not wired into the build yet."
	@echo "roadmap: run it by hand -> python3 tools/esp_mkimage.py $(BIN) $(BUILD)/$(APP).app.bin"

flash: $(BIN)
	@echo "roadmap: our-own ROM SLIP downloader is not implemented yet."
	@echo "roadmap: esp_flash.py will shim esptool as the transport -> python3 tools/esp_flash.py $(BIN)"

sim:
	@echo "roadmap: host-side simulator + companion-IC link -- see sim/README.md"

clean:
	rm -rf $(BUILD)
