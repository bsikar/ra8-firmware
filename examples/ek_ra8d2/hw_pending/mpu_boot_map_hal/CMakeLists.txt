# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMake stub for `mpu_boot_map_hal`. All build logic lives in the shared
# cmake/ra8_add_app.cmake recipe; this file only names the app, its deps, and
# turns on the cache + MPU boot path so the per-app system_init.c installs the
# MPU map through the ra8_mpu HAL.
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

ra8_add_app(
  NAME mpu_boot_map_hal
  STACK_BYTES 2200
  LIBS ra8_board_ek_ra8d2 ra8_mpu
  DESCRIPTION "MPU boot map brought up through the ra8_mpu HAL (#576)"
)

# Turn on the cache + MPU boot path for THIS app only. The per-app
# system_init.c (which overrides the shared board boot) is compiled into this
# executable, so this PRIVATE define reaches that translation unit here and
# nowhere else: SystemInit() installs the boot MPU map via
# ra8_mpu_apply_boot_map() and enables the I-cache / D-cache / branch predictor
# before main() runs.
target_compile_definitions(mpu_boot_map_hal.elf PRIVATE RA8_BOOT_ENABLE_CACHE_MPU)
