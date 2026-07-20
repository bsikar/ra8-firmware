#
# Per-app CMake stub for `ereader_manga`. All build logic lives in the
# shared cmake/ra8_add_app.cmake recipe; this file only names the app + its deps.
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
# below re-runs project() in the standalone case. ra8_jof transitively
# wires the vendored libwebp decoder (#290 normalize-on-import); the bare
# "miniz" token supplies the DEFLATE core that ra8_jof's PNG decoder and
# tile codec inflate/deflate through. Both are C, but the languages keep CXX to
# match the shared toolchain setup used across the example apps.
get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
    project(ereader_manga LANGUAGES C CXX ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
    get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

ra8_add_app(NAME ereader_manga STACK_BYTES 32768
           DESCRIPTION "Viewable manga reader (Demo B): baked PNG page -> JOF tile atlas (ra8_jof_produce) -> tap-zone pan/zoom viewport over the GLCDC panel through a small ra8_tile_cache"
           LIBS ra8_gfx ra8_display_pal ra8_jof ra8_mem ra8_io_bus miniz)
