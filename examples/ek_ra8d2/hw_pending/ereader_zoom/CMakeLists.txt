#
# Per-app CMake stub for `ereader_zoom`. All build logic lives in the
# shared cmake/ra8_add_app.cmake recipe; this file only names the app + its deps.
#
# Standalone:  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8d2.cmake
# Embedded:    pulled in by the top-level CMakeLists via add_subdirectory().
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
cmake_minimum_required(VERSION 3.20)

# A standalone `cmake -S . -B build` makes this file the top-level listfile,
# which CMake requires to contain a literal, direct project() call. The
# aggregate repo-root build instead pulls this app in via add_subdirectory()
# (project() is already called at the root), so guard on PARENT_DIRECTORY to
# avoid a redundant nested sub-project. ra8_add_app() below re-runs project()
# in the standalone case.
#
# The page this demo magnifies is procedural, so unlike its sibling reader
# demos there is no container, no decoder and no vendored codec in the link:
# ra8_zoom (the viewport engine), ra8_gfx (the blue-noise dither + blit),
# ra8_mem (the tile cache that bounds residency), ra8_ui (the rect type) and
# ra8_display_pal (the panel) are the whole dependency set.
get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(ereader_zoom LANGUAGES C CXX ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

ra8_add_app(
  NAME ereader_zoom
  STACK_BYTES 32768
  DESCRIPTION "Tap-to-zoom viewer: full-screen zoom + panel loupe over a tiled 12 MP page"
  LIBS ra8_zoom
       ra8_gfx
       ra8_ui
       ra8_mem
       ra8_display_pal
       ra8_io_bus
)
