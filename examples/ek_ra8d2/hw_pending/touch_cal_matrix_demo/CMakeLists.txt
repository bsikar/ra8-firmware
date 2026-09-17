# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMake stub for `touch_cal_matrix_demo`. All build logic lives in the shared
# cmake/ra8_add_app.cmake recipe; this file only names the app + its deps.
#
# Standalone:  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8d2.cmake
# Embedded:    pulled in by the top-level CMakeLists via add_subdirectory().
#
#
cmake_minimum_required(VERSION 3.20)

get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(touch_cal_matrix_demo LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

# The demo stands up a deterministic fake panel in main.c: the draw shim records
# the cross-hair ra8_touch_cal_run asked for and the read shim answers with the
# raw sample a controller obeying a known affine map would report. Nothing is
# persisted, so no storage backend is exercised and no EXTRA_SRCS are needed.
#
# ra8_io globs every backend/sink (sd-spi -> ra8_sdmmc_spi, usb-cdc ->
# ra8_usb_pal), so those libs are listed to satisfy their includes, same as
# devcfg_record_demo. Boot files come from the shared
# libs/ra8_board_ek_ra8d2/src/boot copy (no local vector_table.c). (#155/#264)
ra8_add_app(
  NAME touch_cal_matrix_demo
  STACK_BYTES 4096
  DESCRIPTION "ra8_touch_cal: five-target run, affine solve, apply, and blob round trip"
  LIBS ra8_touch_cal ra8_io ra8_fs ra8_sdmmc_spi ra8_usb_pal
)
