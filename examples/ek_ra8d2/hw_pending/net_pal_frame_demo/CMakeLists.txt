# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMake stub for `net_pal_frame_demo`. All build logic lives in the
# shared cmake/ra8_add_app.cmake recipe; this file only names the app + deps.
#
# Standalone:  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8d2.cmake
# Embedded:    pulled in by the top-level CMakeLists via add_subdirectory().
#
#
cmake_minimum_required(VERSION 3.20)

get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(net_pal_frame_demo LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

# ra8_net_pal wraps the Ring-3 ra8_eth driver (ESWM MSTP gate + status fan-out).
# ra8_io globs every backend/sink (sd-spi -> ra8_sdmmc_spi, usb-cdc ->
# ra8_usb_pal), so those libs are listed to satisfy their includes.
ra8_add_app(
  NAME net_pal_frame_demo
  STACK_BYTES 4096
  DESCRIPTION "Stack-facing ethernet PAL contract: MAC, frame ring, events, unwind (#621)"
  LIBS ra8_net_pal
       ra8_io
       ra8_fs
       ra8_sdmmc_spi
       ra8_usb_pal
)
