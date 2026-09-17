# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMake stub for `vfs_port_demo`. All build logic lives in the shared
# cmake/ra8_add_app.cmake recipe; this file only names the app + its deps.
#
# Standalone:  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8d2.cmake
# Embedded:    pulled in by the top-level CMakeLists via add_subdirectory().
#
#
cmake_minimum_required(VERSION 3.20)

get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(vfs_port_demo LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

# ra8_io globs every backend/sink (sd-spi -> ra8_sdmmc_spi, usb-cdc -> ra8_usb_pal),
# so those libs are listed only to satisfy their includes; this app's medium is a
# RAM blockdev it owns, which needs nothing extra. The whole demo body lives in
# src/main.c, so no EXTRA_SRCS are needed. Boot files come from the shared
# libs/ra8_board_ek_ra8d2/src/boot copy (no local vector_table.c). (#155/#264)
ra8_add_app(
  NAME vfs_port_demo
  STACK_BYTES 4096
  DESCRIPTION "if_ra8_vfs: portable fw_if_fs facade bound to a RAM/FAT12/VFS volume"
  LIBS if if_ra8_vfs ra8_io ra8_fs ra8_sdmmc_spi ra8_usb_pal
)
