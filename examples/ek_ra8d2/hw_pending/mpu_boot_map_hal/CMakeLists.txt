# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMake stub for `mpu_boot_map_hal`. All build logic lives in the shared
# cmake/ra8_add_app.cmake recipe; this file only names the app, its deps, and
# selects the HAL MPU boot-map gate so the shared board `SystemInit()` installs
# the MPU map through `ra8_mpu_apply_boot_map()`.
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
  project(mpu_boot_map_hal LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

# The Zig spelling of the shared boot unit reads these gates as boot_options
# (RA8FW-365); the target_compile_definitions below stay for the C readers
# (main.c). Set before ra8_add_app() so the boot-object build sees them.
set(RA8_APP_BOOT_CACHE_MPU ON)
set(RA8_APP_BOOT_MPU_VIA_HAL ON)
ra8_add_app(
  NAME mpu_boot_map_hal
  STACK_BYTES 2200
  LIBS ra8_board_ek_ra8d2 ra8_mpu
  DESCRIPTION "MPU boot map brought up through the ra8_mpu HAL"
)

# Turn on the cache + MPU HAL boot path for THIS app only: the shared board
# boot (libs/ra8_board_ek_ra8d2/src/boot/system_init.zig) installs the boot MPU
# map via ra8_mpu_apply_boot_map() and enables I/D-cache + branch predictor.
target_compile_definitions(
  mpu_boot_map_hal.elf PRIVATE RA8_BOOT_ENABLE_CACHE_MPU RA8_BOOT_MPU_VIA_HAL
)
