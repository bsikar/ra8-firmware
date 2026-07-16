#
# Per-app CMake stub for `ereader_webtoon`. All build logic lives in the shared
# cmake/ra8_add_app.cmake recipe; this file only names the app + its deps.
#
# Standalone:  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8d2.cmake
# Embedded:    pulled in by the top-level CMakeLists via add_subdirectory().
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
cmake_minimum_required(VERSION 3.20)

get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
    project(ereader_webtoon LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
    get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

# ra8_webtoon is the #289 continuous vertical-scroll engine; it pages RTA1 bands
# (ra8_tileatlas) through the ra8_tile_cache (ra8_mem) and composites them via
# ra8_gfx_blit. ra8_tileatlas's reader references ra8_io_decompress (deflate
# tiles), which inflates through the vendored miniz -- the ra8_add_app
# tileatlas special-case links ra8_io_compress.c, and bare `miniz` supplies the
# implementation TU. This app builds a raw-codec strip, so decompress is never
# called at runtime, only linked.
ra8_add_app(NAME ereader_webtoon STACK_BYTES 16384
           DESCRIPTION "Headless #289 gate: tall synthetic RTA1 webtoon strip -> continuous vertical scroll (0-skip) -> framebuffer CRC over UART (sim gate)"
           LIBS ra8_webtoon ra8_tileatlas ra8_mem ra8_gfx miniz ra8_board_ek_ra8d2)
