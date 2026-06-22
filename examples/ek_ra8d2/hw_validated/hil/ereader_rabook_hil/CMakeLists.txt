#
# Per-app CMake stub for `ereader_rabook_hil`. All build logic lives in the
# shared cmake/ra_add_app.cmake recipe; this file only names the app + its deps.
#
# Standalone:  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8d2.cmake
# Embedded:    pulled in by the top-level CMakeLists via add_subdirectory().
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

# ra_book provides the compiled-book loader + the DOM->XHTML render bridge;
# ra_reflow lays it out and ra_gfx rasterizes. No ra_epub / miniz: the fixture
# is baked inflated, so the app validates the flat blob directly.
ra_add_app(NAME ereader_rabook_hil STACK_BYTES 16384
           DESCRIPTION "Headless load + render of a compiled .rabook, small to large (HIL gate)"
           LIBS ra_gfx ra_reflow ra_book)
