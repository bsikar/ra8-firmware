#
# Per-app CMake stub for `pagecache`. All build logic lives in the shared
# cmake/ra8_add_app.cmake recipe; this file only names the app + its deps.
#
# Standalone:  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8d2.cmake
# Embedded:    pulled in by the top-level CMakeLists via add_subdirectory().
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
cmake_minimum_required(VERSION 3.20)

# A standalone `cmake -S . -B build` makes this file the top-level
# listfile, which CMake requires to contain a literal, direct project()
# call. The aggregate repo-root build instead pulls this app in via
# add_subdirectory() (project() is already called at the root), so guard on
# PARENT_DIRECTORY to avoid a redundant nested sub-project. ra8_add_app()
# below re-runs project() in the standalone case to apply the shared VERSION
# and the app description.
get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(pagecache LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

ra8_add_app(
  NAME pagecache
  STACK_BYTES 4400
  DESCRIPTION "ra8_reflow pagination-cache round-trip from SD via ra8_fs (#117)"
  LIBS ra8_reflow
       ra8_gfx
       ra8_fs
       ra8_sdmmc_spi
       ra8_board_ek_ra8d2
)

# Bake the Latin-1 subset font (libs/ra8_fonts/literata_latin1.ttf) into .rodata so a
# blank SD card self-provisions FONT.OTF on first boot (write the blob, read it
# back). The hex array is generated at build time -- not committed -- so the
# ~37 KB of byte literals never reach the QC gates. ${_d} is the repo root above.
set(_ra8_baked_font_c "${CMAKE_CURRENT_BINARY_DIR}/ra8_font_literata_latin1.c")
add_custom_command(
  OUTPUT "${_ra8_baked_font_c}"
  COMMAND
    ${CMAKE_COMMAND} -E env python3 "${_d}/scripts/gen/font_to_c.py"
    "${_d}/libs/ra8_fonts/literata_latin1.ttf" "${_ra8_baked_font_c}" g_ra8_font_literata_latin1
    literata_latin1.h
  DEPENDS "${_d}/libs/ra8_fonts/literata_latin1.ttf" "${_d}/scripts/gen/font_to_c.py"
  COMMENT "Generating baked Latin-1 font array (pagecache)"
)
target_sources(pagecache.elf PRIVATE "${_ra8_baked_font_c}")
# libs/ra8_fonts holds the per-font declaration header (literata_latin1.h) that both
# main.c and the generated array .c include.
target_include_directories(pagecache.elf PRIVATE "${_d}/libs/ra8_fonts")
