#
# Per-app CMake stub for `epub_stress`. ra_epub pulls in the vendored miniz
# (ZIP) + tinyxml2 (XML) via the ra_epub special-case in ra_add_app.cmake;
# ra_reflow supplies the stb_truetype glyph glue ra_epub links against, and
# ra_gfx backs ra_reflow's render path.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
cmake_minimum_required(VERSION 3.20)

# A standalone `cmake -S . -B build` makes this file the top-level
# listfile, which CMake requires to contain a literal, direct project()
# call. The aggregate repo-root build instead pulls this app in via
# add_subdirectory() (project() is already called at the root), so guard on
# PARENT_DIRECTORY to avoid a redundant nested sub-project. ra_add_app()
# below re-runs project() in the standalone case to apply the shared VERSION
# and the app description. ra_epub pulls in C++ (tinyxml2 +
# the XML shim), so the languages must include CXX.

get_directory_property(_ra_has_parent PARENT_DIRECTORY)
if(NOT _ra_has_parent)
    project(epub_stress LANGUAGES C CXX ASM)
endif()

set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
    get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra_add_app.cmake")

ra_add_app(NAME epub_stress STACK_BYTES 16384
           DESCRIPTION "epub_stress firmware for RA8D2"
           LIBS ra_epub ra_reflow ra_gfx ra_board_ek_ra8d2)
