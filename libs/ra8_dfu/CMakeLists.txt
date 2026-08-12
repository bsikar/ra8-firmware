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
#      ra8_flash (ra8_dfu_program.c).
#   3. USBX DFU device class wired to real MRAM + the polled host DFU
#      driver (ra8_dfu_device.c / ra8_dfu_host.c), each bound to either USB
#      controller through the ux_dcd_ra8_usb bridge via ra8_usb_speed_t.
#      These two TUs are firmware-only (guarded by RA8_OFF_TARGET) so
#      the host test build compiles them away.
#
# The module never calls malloc -- all buffers live in static storage.
#
# Picked up automatically by tests/CMakeLists.txt globbing; apps pull it
# in with ra8_add_app(... LIBS ra8_dfu).
#
#

# Intentionally empty -- consumers reach into inc/ + src/ directly.
