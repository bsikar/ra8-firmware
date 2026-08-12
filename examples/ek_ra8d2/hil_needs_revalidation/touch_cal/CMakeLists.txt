# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMake stub for `touch_cal`. All build logic lives in the shared
# cmake/ra8_add_app.cmake recipe; this file only names the app + its deps. The
# app drives three first-party libraries: ra8_touch_cal (the affine-fit solver),
# ra8_io_bus (the I2C bus facade the GT911 touch driver reads through, same as
# touch_demo), and ra8_display_pal (the GLCDC LCD backend that paints the
# calibration crosshairs, same as glcdc_render). ra8_touch (the GT911 driver)
# and ra8_glcdc live in ra8_hal, which the recipe links into every app.
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
  project(touch_cal LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

ra8_add_app(
  NAME touch_cal
  STACK_BYTES 4096
  LIBS ra8_touch_cal ra8_io_bus ra8_display_pal ra8_board_ek_ra8d2
  DESCRIPTION "N-point affine touch calibration (GT911 + GLCDC) for RA8D2"
)
