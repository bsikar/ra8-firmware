#
# Per-app CMake stub for `modem_at_demo`. All build logic lives in the shared
# cmake/ra8_add_app.cmake recipe; this file only names the app + its deps.
#
# Standalone:  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8d2.cmake
# Embedded:    pulled in by the top-level CMakeLists via add_subdirectory().
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
cmake_minimum_required(VERSION 3.20)

# A standalone `cmake -S . -B build` makes this file the top-level listfile,
# which CMake requires to contain a literal, direct project() call. The
# aggregate repo-root build instead pulls this app in via add_subdirectory()
# (project() is already called at the root), so guard on PARENT_DIRECTORY to
# avoid a redundant nested sub-project. ra8_add_app() below re-runs project() in
# the standalone case to apply the shared VERSION and the app description.
get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(modem_at_demo LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

# ra8_modem_at: the cellular AT command/response driver this demo exercises over
# the MikroBUS UART (SCI7). The ra8_sci HAL + ra8_board console it uses are
# globbed in with ra8_hal / the board layer automatically.
ra8_add_app(
  NAME modem_at_demo
  STACK_BYTES 4096
  DESCRIPTION "Cellular AT modem bring-up + CSQ/CREG/CGATT query over SCI7 (#259)"
  LIBS ra8_modem_at
)
