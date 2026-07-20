#
# Per-app CMake stub for `dma_coherency_hil`. All build logic lives in the shared
# cmake/ra8_add_app.cmake recipe; this file only names the app and turns on the
# Cortex-M85 cache + MPU boot path so the cache-maintenance hazard is real.
#
# Standalone:  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8d2.cmake
# Embedded:    pulled in by the top-level CMakeLists via add_subdirectory().
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
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
  project(dma_coherency_hil LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

ra8_add_app(
  NAME dma_coherency_hil
  STACK_BYTES 2200
  LIBS ra8_board_ek_ra8d2
  DESCRIPTION "dma_coherency_hil firmware for RA8D2"
)

# Turn ON the Cortex-M85 D-cache / I-cache + MPU boot path for THIS app only.
# The shared boot (libs/ra8_board_ek_ra8d2/boot/system_init.c) is compiled
# per-app, so this define reaches only dma_coherency_hil.elf. With the D-cache
# enabled the DMAC mem-to-mem transfer is no longer automatically coherent with
# the M85 -- the app must clean src before the DMAC reads it and invalidate dst
# after the DMAC writes it. That hazard is the whole point of this HIL test.
target_compile_definitions(dma_coherency_hil.elf PRIVATE RA8_BOOT_ENABLE_CACHE_MPU)

# Keep INFO-level logging in this RelWithDebInfo HIL build so the PASS/FAIL
# result is also emitted over ITM (additive to the VCOM banner), which
# board_sim surfaces on its [itm] tap.
target_compile_definitions(dma_coherency_hil.elf PRIVATE RA8_LOG_LEVEL=RA8_LOG_LEVEL_INFO)
