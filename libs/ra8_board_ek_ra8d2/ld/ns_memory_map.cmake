# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# The EK-RA8D2 Non-Secure memory map: ONE definition of the addresses that were
# repeated across two hand-maintained linker scripts (#759 item 2).
#
# Two consumers, one definition:
#   * ra8_add_ns_image() configures ns_image.ld.in into the build tree with
#     these values, so the NS link gets its MEMORY block from here.
#   * ra8_ns_memory_map_defines(<target>) puts them on a SECURE target as
#     compile definitions, so trustzone_init.c's k_tz_ns_load_base /
#     k_tz_ns_run_base stop being a hand-copy. A Secure image that forgets the
#     call fails to compile on an undeclared RA8_NS_* rather than linking with
#     a quietly stale address.

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

# Put the NS memory map on a Secure target as compile definitions.
#
# The NS image is a separate ELF, so the Secure side has none of its linker
# symbols and genuinely has to carry the addresses as constants. What it does
# not have to do is re-type them: this is the same set the NS linker script is
# generated from.
function(ra8_ns_memory_map_defines _target)
  if(NOT TARGET ${_target})
    message(FATAL_ERROR "ra8_ns_memory_map_defines(): no such target ${_target}")
  endif()
  target_compile_definitions(
    ${_target}
    PRIVATE RA8_NS_MRAM_BASE=${RA8_NS_MRAM_ORIGIN}U
            RA8_NS_OSPI_BASE=${RA8_NS_OSPI_ORIGIN}U
            RA8_NS_SRAM_BASE=${RA8_NS_SRAM_ORIGIN}U
  )
endfunction()
