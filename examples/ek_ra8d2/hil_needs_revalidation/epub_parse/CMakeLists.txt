# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMake stub for `epub_parse`. ra8_epub pulls in the vendored miniz
# (ZIP) + tinyxml2 (XML) via the ra8_epub special-case in ra8_add_app.cmake;
# ra8_reflow supplies the stb_truetype glyph glue ra8_epub links against, and
# ra8_gfx backs ra8_reflow's render path.
#
#
cmake_minimum_required(VERSION 3.20)

set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
# A standalone `cmake -S . -B build` makes this file the top-level
# listfile, which CMake requires to contain a literal, direct project()
# call. The aggregate repo-root build instead pulls this app in via
# add_subdirectory() (project() is already called at the root), so guard on
# PARENT_DIRECTORY to avoid a redundant nested sub-project. ra8_add_app()
# below re-runs project() in the standalone case to apply the shared VERSION
# and the app description. ra8_epub pulls in C++ (tinyxml2 + the XML shim), so
# the languages must include CXX or those TUs are silently skipped.
get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(epub_parse LANGUAGES C CXX ASM)
endif()

include("${_d}/cmake/ra8_add_app.cmake")

ra8_add_app(
  NAME epub_parse
  STACK_BYTES 16384
  DESCRIPTION "epub_parse firmware for RA8D2"
  LIBS ra8_epub ra8_reflow ra8_gfx ra8_board_ek_ra8d2
)
