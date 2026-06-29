#
# Per-app CMake stub for `ra_io_cache_demo`. All build logic lives in the shared
# cmake/ra_add_app.cmake recipe; this file only names the app + its deps.
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
    project(ra_io_cache_demo LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
    get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra_add_app.cmake")

# ra_io globs every backend/sink (sd-spi -> ra_sdmmc_spi, usb-cdc -> ra_usb_pal),
# so those libs are listed to satisfy their includes even though this demo only
# drives the cached RAM backend + UART stream.
ra_add_app(NAME ra_io_cache_demo STACK_BYTES 4096
           DESCRIPTION "ra_io caching block device: LRU sector cache over a RAM disk, re-read hit counters (#160)"
           LIBS ra_io ra_fs ra_sdmmc_spi ra_usb_pal)
