#
# Per-app CMake stub for `pagecache_hil`. All build logic lives in the shared
# cmake/ra_add_app.cmake recipe; this file only names the app + its deps.
#
# Standalone:  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8d2.cmake
# Embedded:    pulled in by the top-level CMakeLists via add_subdirectory().
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
cmake_minimum_required(VERSION 3.20)

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
    get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra_add_app.cmake")

ra_add_app(NAME pagecache_hil STACK_BYTES 4400
           DESCRIPTION "ra_reflow pagination-cache round-trip from SD via ra_fs (#117)"
           LIBS ra_reflow ra_gfx ra_fs ra_sdmmc_spi)

# Bake the Latin-1 subset font (libs/fonts/arnopro_latin1.otf) into .rodata so a
# blank SD card self-provisions FONT.OTF on first boot (write the blob, read it
# back). The hex array is generated at build time -- not committed -- so the
# 56 KB of byte literals never reach the QC gates. ${_d} is the repo root above.
set(_ra_baked_font_c "${CMAKE_CURRENT_BINARY_DIR}/ra_font_arnopro_latin1.c")
add_custom_command(
    OUTPUT "${_ra_baked_font_c}"
    COMMAND ${CMAKE_COMMAND} -E env python3 "${_d}/scripts/utils/font_to_c.py"
            "${_d}/libs/fonts/arnopro_latin1.otf" "${_ra_baked_font_c}"
            g_ra_font_arnopro_latin1 arnopro_latin1.h
    DEPENDS "${_d}/libs/fonts/arnopro_latin1.otf" "${_d}/scripts/utils/font_to_c.py"
    COMMENT "Generating baked Latin-1 font array (pagecache_hil)")
target_sources(pagecache_hil.elf PRIVATE "${_ra_baked_font_c}")
# libs/fonts holds the per-font declaration header (arnopro_latin1.h) that both
# main.c and the generated array .c include.
target_include_directories(pagecache_hil.elf PRIVATE "${_d}/libs/fonts")
