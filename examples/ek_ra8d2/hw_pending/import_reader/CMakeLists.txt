#
# Per-app CMake stub for `import_reader`. All build logic lives in the shared
# cmake/ra_add_app.cmake recipe; this file only names the app + its deps.
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
# avoid a redundant nested sub-project. ra_add_app() below re-runs project() in
# the standalone case to apply the shared VERSION and the app description.
# ra_epub + the ra_rabook_compile XML shim are C++ (tinyxml2 + the shim), so the
# languages must include CXX.
get_directory_property(_ra_has_parent PARENT_DIRECTORY)
if(NOT _ra_has_parent)
    project(import_reader LANGUAGES C CXX ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
    get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra_add_app.cmake")

# ra_rabook_import is the on-import compile-and-cache manager (#151) plus its
# production compile adapter (ra_rabook_import_compile_adapter). The adapter
# drives ra_epub_open_fs + ra_rabook_compile (the EPUB -> RABOOK1 pipeline), so
# both libraries are listed. ra_book reads the resulting flat .rabook back. The
# compile pipeline decodes cover art via the vendored stb_image and an
# ra_img_arena bump arena, both wired in by ra_reflow (ra_gfx is ra_reflow's
# rasteriser dependency); ra_fs over ra_sdmmc_spi is the SD-card storage.
#
# The LIBS loop in ra_add_app() globs only `*.c`, so ra_rabook_compile's lone
# C++ TU (the lenient XHTML -> DOM shim, ra_rabook_xml_shim.cpp) is wired
# explicitly here -- mirroring the way ra_add_app() special-cases ra_epub's
# C++ shim.
ra_add_app(NAME import_reader STACK_BYTES 16384
           DESCRIPTION "On-import EPUB->.rabook compile+cache on SD, then read the cached book (#151)"
           LIBS ra_rabook_import ra_rabook_compile ra_epub ra_book ra_reflow ra_gfx
                ra_fs ra_sdmmc_spi ra_board_ek_ra8d2
           EXTRA_SRCS ${_d}/libs/ra_rabook_compile/src/ra_rabook_xml_shim.cpp)
