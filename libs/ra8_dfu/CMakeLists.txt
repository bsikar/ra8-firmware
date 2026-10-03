# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# libs/ra8_dfu -- controller-agnostic USB-DFU MRAM bootloader core.
#
# Provides:
#   1. Pure boot logic (ra8_dfu_boot.c): software CRC32, image-header
#      validation, A/B slot selection, and the reset-time boot decision.
#      No MMIO / no USB -- host-unit-testable.
#   2. SRAM-resident MRAM program/verify of the inactive slot over
#      ra8_flash, which is Zig now: src/program_abi.zig behind the
#      unchanged inc/ra8_dfu.h. Its exports carry linksection(".sram_text"),
#      so an app still reaches them through ra8_add_app(SRAM_TEXT ...) and
#      only ra8_flash.c has to be named there by file.
#   3. The USBX DFU device class glue moved to its own Zig archive,
#      libs/ra8_dfu_device (RA8FW-507). Linking it is the opt-in to USBX,
#      so apps that call ra8_dfu_device_start name both in LIBS.
#   4. The polled host-side DFU driver, which is Zig now:
#      src/dfu_host_abi.zig behind the unchanged inc/ra8_dfu_host.h, built by
#      build.zig. Firmware-only by construction rather than by guard, since
#      the only HAL binding it names is the real ra8_usb_host_* seam.
#
# The module never calls malloc -- all buffers live in static storage.
#
# Picked up automatically by tests/CMakeLists.txt globbing; apps pull it
# in with ra8_add_app(... LIBS ra8_dfu), which also registers the Zig
# archive: libs/ra8_dfu has a build.zig and no src/ra8_dfu.c, which is the
# condition cmake/ra8_app/sources.cmake keys the archive on.
#
#

# Intentionally empty -- consumers reach into inc/ + src/ directly.
