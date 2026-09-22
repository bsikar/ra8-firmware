# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMake stub for `eth_open_probe`. All build logic lives in the
# shared cmake/ra8_add_app.cmake recipe; this file only names the app.
#
# Standalone:  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8d2.cmake
# Embedded:    pulled in by the top-level CMakeLists via add_subdirectory().
#
#
cmake_minimum_required(VERSION 3.20)

# A standalone `cmake -S . -B build` makes this file the top-level
# listfile, which CMake requires to contain a literal, direct project()
# call. The aggregate repo-root build instead pulls this app in via
# add_subdirectory() (project() is already called at the root), so guard on
# PARENT_DIRECTORY to avoid a redundant nested sub-project. ra8_add_app()
# below re-runs project() in the standalone case to apply the shared VERSION
# and the app description.
get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(eth_open_probe LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

ra8_add_app(
  NAME eth_open_probe
  STACK_BYTES 2200
  LIBS ra8_board_ek_ra8d2
  DESCRIPTION "Bare-metal ra8_eth_open bring-up probe for RA8D2 (issue #524)"
)

# Address-sweep knob. The app forces a pad of this many bytes into the plain
# `.bss` input section, which the shared linker script emits ahead of every
# `-fdata-sections` `.bss.*`, so growing it slides the HAL's whole GWCA block
# (state, descriptor chains, buffer pools) that far up SRAM and changes
# nothing else. Must stay a multiple of 16 and non-zero -- static_assert in
# main.c enforces both. Override per build:
#
#   cmake -S . -B build ... -DRA8_ETH_PROBE_PAD_BYTES=16
#
# The default is not 16. 393168 (0x5FFD0) reproduces the exact .bss layout
# that #524 reported -- s_gwca_state at 0x22060474, s_tx_chain at 0x220664B0 --
# which on stock `dev` before the SRAM wait state landed BusFaulted 5 runs out
# of 5, and passes 5 out of 5 with it. Keeping the default there means the
# default build of this app is the reproducer, not a variant of it.
set(RA8_ETH_PROBE_PAD_BYTES
    393168
    CACHE STRING "Leading .bss pad in bytes -- slides the Ethernet statics up SRAM"
)
target_compile_definitions(
  eth_open_probe.elf PRIVATE RA8_ETH_PROBE_PAD_BYTES=${RA8_ETH_PROBE_PAD_BYTES}u
)
