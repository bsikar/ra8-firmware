# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMake stub for `ereader_longstrip`. All build logic lives in the shared
# cmake/ra8_add_app.cmake recipe; this file only names the app + its deps.
#
# Standalone:  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8d2.cmake
# Embedded:    pulled in by the top-level CMakeLists via add_subdirectory().
#
#
cmake_minimum_required(VERSION 3.20)

get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(ereader_longstrip LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

# ereader_longstrip is the #289 continuous vertical-scroll reader made VIEWABLE:
# the ra8_longstrip engine's band-composite blit sink is bound to the live
# 1024x600 GLCDC panel framebuffer (ra8_display_pal + k_display_backend_lcd_
# ra8_glcdc, in external SDRAM) instead of an internal scratch buffer, so
# ra8_emulator's --view / --ppm shows the real reader screen. A tall colourful
# strip is streamed band-by-band through a deliberately small ra8_tile_cache
# (ra8_mem) whose decode-on-miss seam paints each band procedurally, so the
# eviction / bounded-memory path runs with no multi-megabyte atlas. Navigation
# is tap-zone only (GT911 via the ra8_io_bus I2C facade): bottom third = page
# down, top third = page up, centre = toggle chrome.
#
# ra8_jof's reader references ra8_io_decompress (deflate tiles), which
# inflates through the vendored miniz -- the ra8_add_app jof special-case
# links ra8_io_compress.c, and bare `miniz` supplies the implementation TU. This
# app builds a raw-codec atlas header only (procedural bands), so decompress is
# never called at runtime, only linked.
ra8_add_app(
  NAME ereader_longstrip
  STACK_BYTES 16384
  DESCRIPTION "Vertical-scroll strip on the GLCDC panel, tap-zone paging (#289)"
  LIBS ra8_longstrip
       ra8_jof
       ra8_mem
       ra8_gfx
       ra8_display_pal
       ra8_io_bus
       miniz
)
