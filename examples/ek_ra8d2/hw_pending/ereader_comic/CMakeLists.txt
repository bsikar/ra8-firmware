# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMake stub for `ereader_comic`. All build logic lives in the
# shared cmake/ra8_add_app.cmake recipe; this file only names the app + its deps.
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
# below re-runs project() in the standalone case. ra8_comic's CBZ backend needs
# miniz built WITH its ZIP reader + the ra8_epub_miniz_alloc static pool, both of
# which the `ra8_epub` LIBS path supplies -- and ra8_epub pulls in C++ (tinyxml2 +
# the XML shim), so the languages must include CXX.
get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(ereader_comic LANGUAGES C CXX ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

# cm_tiled_check.c (#344) opens an oversized baked CBZ page through the JOF tile
# path (ra8_comic_tiles -> ra8_jof produce/read + ra8_tile_cache), so the app
# also pulls ra8_jof + ra8_mem (the tile cache). miniz comes in via ra8_epub.
ra8_add_app(
  NAME ereader_comic
  STACK_BYTES 32768
  DESCRIPTION "Page-turn comic reader: baked CBZ to the GLCDC panel, tap-zone nav"
  EXTRA_SRCS cm_tiled_check.c
  LIBS ra8_gfx
       ra8_reflow
       ra8_comic
       ra8_epub
       ra8_jof
       ra8_mem
       ra8_display_pal
       ra8_io_bus
       ra8_board_ek_ra8d2
)
