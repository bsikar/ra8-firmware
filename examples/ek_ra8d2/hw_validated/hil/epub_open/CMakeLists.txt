#
# Per-app CMake stub for `epub_open`. It joins the SD storage stack
# (ra8_fs FAT layer over the ra8_sdmmc_spi SPI-mode driver) to the EPUB parse
# stack: ra8_epub pulls in the vendored miniz (ZIP) + tinyxml2 (XML) via the
# ra8_epub special-case in ra8_add_app.cmake, ra8_reflow supplies the stb_truetype
# glyph glue ra8_epub links against, and ra8_gfx backs ra8_reflow's render path.
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
# and the app description. ra8_epub pulls in C++ (tinyxml2 +
# the XML shim), so the languages must include CXX.

get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(epub_open LANGUAGES C CXX ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

ra8_add_app(
  NAME epub_open
  STACK_BYTES 16384
  DESCRIPTION "Open + parse a real .epub from SD via ra8_fs (#114)"
  LIBS ra8_epub
       ra8_reflow
       ra8_gfx
       ra8_fs
       ra8_sdmmc_spi
       ra8_board_ek_ra8d2
)
