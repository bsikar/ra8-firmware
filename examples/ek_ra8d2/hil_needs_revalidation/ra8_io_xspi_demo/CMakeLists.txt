# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMake stub for `ra8_io_xspi_demo`. All build logic lives in the shared
# cmake/ra8_add_app.cmake recipe; this file only names the app + its deps.
#
# Standalone:  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8d2.cmake
# Embedded:    pulled in by the top-level CMakeLists via add_subdirectory().
#
#
cmake_minimum_required(VERSION 3.20)

get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(ra8_io_xspi_demo LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

# ra8_io globs every backend/sink (sd-spi -> ra8_sdmmc_spi, usb-cdc -> ra8_usb_pal),
# so those libs are listed to satisfy their includes; the OSPI NOR backend rides
# on ra8_xspi_init, which is in ra8_hal (always linked), so no extra lib is needed.
# EXTRA_SRCS pulls in the backend-agnostic round-trip (self-contained local copy); this
# app differs from the RAM / SDRAM / SD demos only by the OSPI bring-up + its one
# backend bind line + its own PASS banner (#155).
ra8_add_app(
  NAME ra8_io_xspi_demo
  STACK_BYTES 4096
  DESCRIPTION "ra8_io over OSPI NOR: xSPI block dev to FAT to VFS to UART (#155/#156)"
  LIBS ra8_io ra8_fs ra8_sdmmc_spi ra8_usb_pal
  EXTRA_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/ra8_io_roundtrip.c
)
