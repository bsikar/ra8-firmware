#
# Per-app CMake stub for `ra_io_compress_demo`. All build logic lives in the
# shared cmake/ra_add_app.cmake recipe; this file only names the app + its deps.
#
# Standalone:  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8d2.cmake
# Embedded:    pulled in by the top-level CMakeLists via add_subdirectory().
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
cmake_minimum_required(VERSION 3.20)

get_directory_property(_ra_has_parent PARENT_DIRECTORY)
if(NOT _ra_has_parent)
    project(ra_io_compress_demo LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
    get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra_add_app.cmake")

# `miniz` opts this app into ra_io's compression layer: it pulls in the vendored
# heap-free DEFLATE core and un-gates libs/ra_io/src/ra_io_compress.c (otherwise
# filtered out of the ra_io glob for plain consumers). ra_sdmmc_spi / ra_usb_pal
# satisfy the other ra_io backend/sink includes even though this demo only drives
# the RAM backend + UART stream.
ra_add_app(NAME ra_io_compress_demo STACK_BYTES 4096
           DESCRIPTION "ra_io fabric + DEFLATE: RAM disk -> FAT -> compress -> store -> inflate -> verify (#161)"
           LIBS ra_io miniz ra_fs ra_sdmmc_spi ra_usb_pal)
