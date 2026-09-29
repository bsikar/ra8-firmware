# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMake stub for `camera_io_stream_demo`. All build logic lives in the
# shared cmake/ra8_add_app.cmake recipe; this file only names the app + its deps.
#
# Standalone:  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8d2.cmake
# Embedded:    pulled in by the top-level CMakeLists via add_subdirectory().
#
cmake_minimum_required(VERSION 3.20)

get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(camera_io_stream_demo LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

# `ra8_camera_io` is a headers+src bridge with no CMake target of its own, so an
# app opts in by listing it next to `ra8_camera` and `ra8_io` (its own
# CMakeLists.txt says exactly that). ra8_io globs every backend/sink (sd-spi ->
# ra8_sdmmc_spi, usb-cdc -> ra8_usb_pal), so those libs are listed to satisfy
# their includes, same as ra8_io_mram_demo.
ra8_add_app(
  NAME camera_io_stream_demo
  STACK_BYTES 4096
  DESCRIPTION "ra8_camera_io: encoded frame -> ra8_io_stream sink, self-checked (#917)"
  LIBS ra8_camera_io ra8_camera ra8_io ra8_fs ra8_sdmmc_spi ra8_usb_pal ra8_board_ek_ra8d2
)
