#
# Per-app CMake stub for `c6_hosted_init`. All build logic lives in the shared
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
  project(c6_hosted_init LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

# USES pulls in ThreadX (the OS-abstraction vtable is implemented on it) and
# the vendored esp-hosted host driver plus its first-party RA8 port. LIBS
# globs the board layer in so the app reads its pin map from the board rather
# than re-encoding EK-RA8D2 pin facts. The ra8_io SPI bus facade the port
# drives arrives with the port's own source list, not through LIBS: globbing
# the whole of ra8_io would drag the block-device and file-system layers in
# behind it, none of which this app uses.
ra8_add_app(
  NAME c6_hosted_init
  STACK_BYTES 4096
  DESCRIPTION "Bring the esp-hosted RA8 port up against the ESP32-C6 over SPI"
  USES threadx esp_hosted
  LIBS ra8_board_ek_ra8d2
)
