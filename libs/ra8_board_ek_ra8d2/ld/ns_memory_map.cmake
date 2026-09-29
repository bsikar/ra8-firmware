# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# The EK-RA8D2 Non-Secure memory map: ONE definition of the addresses that were
# repeated across two hand-maintained linker scripts (#759 item 2).
#
# Consumed by ra8_add_ns_image(), which configures ns_image.ld.in into the build
# tree with these values. The Secure side still open-codes the same numbers in
# trustzone_init.c (k_tz_ns_load_base / k_tz_ns_run_base); wiring those to these
# variables is the remaining half of "one definition" and needs a change to the
# Secure boot rather than to the link.

# No include_guard: this module DEFINES VARIABLES rather than functions, and a
# global guard would make every include after the first a silent no-op, leaving
# the caller's scope empty. Re-including is the point -- each ra8_ns_linker_script()
# call needs these in its own scope.

# Load home in Secure MRAM -- the flasher writes physical MRAM here, and the
# Secure boot copies the window to the SRAM run alias before BLXNS.
set(RA8_NS_MRAM_ORIGIN 0x02080000)
set(RA8_NS_MRAM_LENGTH 256K)

# Execute-in-place home: OSPI flash, addressed through the TrustZone Non-secure
# alias (IDAU bit[28] = 1) of the 0x8000_0000 physical XIP window.
set(RA8_NS_OSPI_ORIGIN 0x90000000)
set(RA8_NS_OSPI_LENGTH 256K)

# Writable run-time home: SRAM2 via the bit[28] Non-secure alias.
set(RA8_NS_SRAM_ORIGIN 0x32100000)
set(RA8_NS_SRAM_LENGTH 512K)
