#
# Per-app CMake stub for `epub_stress_hil`. ra_epub pulls in the vendored miniz
# (ZIP) + tinyxml2 (XML) via the ra_epub special-case in ra_add_app.cmake;
# ra_reflow supplies the stb_truetype glyph glue ra_epub links against, and
# ra_gfx backs ra_reflow's render path.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
cmake_minimum_required(VERSION 3.20)

set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
    get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra_add_app.cmake")

ra_add_app(NAME epub_stress_hil STACK_BYTES 16384
           DESCRIPTION "epub_stress_hil firmware for RA8D2"
           LIBS ra_epub ra_reflow ra_gfx)
