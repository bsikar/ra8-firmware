#
# libs/ra8_devcfg -- per-device (per-unit) configuration record.
#
# Provides:
#   1. A versioned, CRC-32-protected record schema (VCOM, serial, touch/panel
#      calibration, device-key identity) with a two-copy, header-last commit
#      for power-fail atomicity and a boot-time resolver (ra8_devcfg.c).
#      No MMIO -- reached only through the injected ra8_devcfg_store_t seam,
#      so every branch is host-unit-testable.
#   2. The production extra-MRAM store binding
#      (ra8_devcfg_store_extra_mram.c): reads the memory-mapped data-flash
#      window, writes through ra8_flash_extra_mram_write. Under
#      RA8_SIMULATOR_MODE both accessors address a RAM shadow so the host
#      build exercises the same control flow without MMIO.
#
# The module never calls malloc -- all buffers live on the stack or in static
# storage.
#
# Picked up automatically by tests/CMakeLists.txt globbing; apps pull it in
# with ra8_add_app(... LIBS ra8_devcfg).
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

# Intentionally empty -- consumers reach into inc/ + src/ directly.
