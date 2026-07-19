#
# Per-app CMake stub for `epaper_refresh`. All build logic lives in the shared
# cmake/ra8_add_app.cmake recipe; this file only names the app + its deps.
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
    project(epaper_refresh LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
    get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

# ra8_display_pal: the e-ink (IT8951) backend + PAL. ra8_io_bus: the ra8_io
# SPI-bus facade that binds the ra8_epaper bus seam. ra8_epd_cal: the VCOM
# resolution policy that decides whether this panel may be driven at all
# (INV-VCOM-1) -- without it the driver refuses every refresh, by design.
# The ra8_epaper HAL driver is globbed in with ra8_hal automatically.
ra8_add_app(NAME epaper_refresh STACK_BYTES 4096
           DESCRIPTION "IT8951 e-paper full + partial refresh via the display PAL (#256)"
           LIBS ra8_display_pal ra8_epd_cal ra8_io_bus)
