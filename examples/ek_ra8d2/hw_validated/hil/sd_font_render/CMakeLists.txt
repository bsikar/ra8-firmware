# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMake stub for `sd_font_render`. All build logic lives in the shared
# cmake/ra8_add_app.cmake recipe; this file only names the app + its deps.
#
# Standalone:  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8d2.cmake
# Embedded:    pulled in by the top-level CMakeLists via add_subdirectory().
#
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
  project(sd_font_render LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

ra8_add_app(
  NAME sd_font_render
  STACK_BYTES 4400
  DESCRIPTION "Load a TTF/OTF font off an SD card and render it with reflow"
  LIBS ra8_gfx
       ra8_display_pal
       reflow
       ra8_sdfont
       ra8_sdmmc_spi
       ra8_fs
)

# Bake the Latin-1 subset font (libs/ra8_fonts/literata_latin1.ttf) into .rodata so a
# blank "random" SD card self-provisions FONT.OTF on first boot: ra8_sdfont writes
# this blob to the card, then reads it back. The hex array is generated at build
# time -- not committed -- so the ~37 KB of byte literals never reach the QC gates.
# ${_d} is the repo root resolved above.
# The font baker moved from Python to Zig (#940, epic #858); it is built for the
# HOST with the pinned toolchain even in this Arm cross build, and a missing zig
# is a configure-time error rather than a silently skipped bake.
include("${_d}/cmake/ra8_zig_tool.cmake")
ra8_add_zig_host_tool(NAME font_to_c ZIG_ROOT "${_d}/tools/font_to_c")
set(_ra8_baked_font_c "${CMAKE_CURRENT_BINARY_DIR}/ra8_font_literata_latin1.c")
add_custom_command(
  OUTPUT "${_ra8_baked_font_c}"
  COMMAND
    $<TARGET_FILE:ra8_zig::font_to_c> "${_d}/libs/ra8_fonts/literata_latin1.ttf"
    "${_ra8_baked_font_c}" g_ra8_font_literata_latin1 literata_latin1.h
  # The baker binary itself is a FILE input: a custom TARGET in DEPENDS is only
  # order-only under Ninja, so without $<TARGET_FILE:...> a rebuilt font_to_c
  # would leave the previously baked array in place.
  DEPENDS "${_d}/libs/ra8_fonts/literata_latin1.ttf" "$<TARGET_FILE:ra8_zig::font_to_c>"
          ra8_zig_tool_font_to_c
  COMMENT "Generating baked Latin-1 font array (sd_font_render)"
)
target_sources(sd_font_render.elf PRIVATE "${_ra8_baked_font_c}")
# libs/ra8_fonts holds the per-font declaration header (literata_latin1.h) that both
# main.c and the generated array .c include.
target_include_directories(sd_font_render.elf PRIVATE "${_d}/libs/ra8_fonts")
