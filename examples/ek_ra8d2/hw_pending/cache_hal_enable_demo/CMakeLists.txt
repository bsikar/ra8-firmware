# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMake stub for `cache_hal_enable_demo`. All build logic lives in the
# shared cmake/ra8_add_app.cmake recipe; this file only names the app and turns
# on the cache + MPU boot path AND the HAL cache-enable route for this app alone.
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
  project(cache_hal_enable_demo LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

ra8_add_app(
  NAME cache_hal_enable_demo
  STACK_BYTES 2200
  LIBS ra8_board_ek_ra8d2
  DESCRIPTION "M85 L1 cache brought up through the ra8_cache HAL for RA8D2"
)

# Turn on the shared boot's cache + MPU bring-up for THIS app only, and select
# the HAL cache-enable route. The shared boot
# (libs/ra8_board_ek_ra8d2/src/boot/system_init.c) is compiled into each app's own
# executable, so these PRIVATE defines reach that translation unit here and
# nowhere else: SystemInit() programmes the 5-region MPU and then enables the
# I-cache / D-cache through ra8_cache_icache_enable() / ra8_cache_dcache_enable()
# (RA8_BOOT_CACHE_VIA_HAL) instead of the hand-rolled register pokes.
target_compile_definitions(
  cache_hal_enable_demo.elf PRIVATE RA8_BOOT_ENABLE_CACHE_MPU RA8_BOOT_CACHE_VIA_HAL
)
