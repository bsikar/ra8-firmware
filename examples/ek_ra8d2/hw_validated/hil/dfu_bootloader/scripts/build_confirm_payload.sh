#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# build_confirm_payload.sh -- build the confirming copy-to-run sample payload.
#
# Links confirm_payload.c at k_ra8_dfu_run_base (0x22020000) against
# libra8_dfu_boot.a and ra8_hal flash/wdt units so the sample payload
# performs its self-test and calls ra8_dfu_confirm().
#
# Usage: examples/ek_ra8d2/hw_validated/hil/dfu_bootloader/scripts/build_confirm_payload.sh [build_dir] [out.bin]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
APP_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$APP_DIR/../../../../.." && pwd)"
BLD="${1:-/tmp/confirm_payload_build}"
OUT="${2:-$BLD/confirm_payload.bin}"
ELF="${OUT%.bin}.elf"

CC="${ARM_CC:-arm-none-eabi-gcc}"
OBJCOPY="${ARM_OBJCOPY:-arm-none-eabi-objcopy}"

mkdir -p "$BLD" "$(dirname "$OUT")"

# Build libra8_dfu_boot.a, libra8_hal.a, and libra8_core_zig.a for cortex_m85 via Zig.
(cd "$REPO_ROOT/libs/ra8_dfu" && \
  zig build -Dtarget=thumb-freestanding-eabihf -Dcpu=cortex_m85 -Doptimize=ReleaseSmall -p "$BLD/dfu")
(cd "$REPO_ROOT/libs/ra8_hal" && \
  zig build -Dtarget=thumb-freestanding-eabihf -Dcpu=cortex_m85 -Doptimize=ReleaseSmall -p "$BLD/hal")
(cd "$REPO_ROOT/libs/ra8_core" && \
  zig build -Dtarget=thumb-freestanding-eabihf -Dcpu=cortex_m85 -Doptimize=ReleaseSmall -p "$BLD/core")

"$CC" -mcpu=cortex-m85 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16 \
  -std=c2x -Os -ffreestanding -nostdlib -nostartfiles -DRA8_FREESTANDING \
  -fno-unwind-tables -fno-asynchronous-unwind-tables \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -Wall -Wextra -Werror \
  -I"$REPO_ROOT/libs/ra8_dfu/inc" \
  -I"$REPO_ROOT/libs/ra8_hal/inc" \
  -I"$REPO_ROOT/libs/ra8_hal/src" \
  -I"$REPO_ROOT/libs/ra8_core/inc" \
  -T "$APP_DIR/payload/confirm_payload.ld" \
  "$APP_DIR/payload/confirm_payload.c" \
  "$REPO_ROOT/libs/ra8_hal/src/ra8_flash.c" \
  -Wl,--start-group \
    "$BLD/dfu/lib/libra8_dfu_boot.a" \
    "$BLD/hal/lib/libra8_hal.a" \
    "$BLD/core/lib/libra8_core.a" \
    -lgcc \
  -Wl,--end-group \
  -o "$ELF"

"$OBJCOPY" -O binary "$ELF" "$OUT"
echo "confirm_payload: $OUT ($(wc -c <"$OUT" | tr -d ' ') bytes), elf: $ELF"
