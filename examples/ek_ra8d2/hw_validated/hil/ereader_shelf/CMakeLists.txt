#
# Per-app CMake stub for `ereader_shelf`. All build logic lives in the shared
# cmake/ra_add_app.cmake recipe; this file only names the app + its deps.
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
# and the app description.
get_directory_property(_ra_has_parent PARENT_DIRECTORY)
if(NOT _ra_has_parent)
    project(ereader_shelf LANGUAGES C CXX ASM)
endif()

set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
    get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra_add_app.cmake")

# Hybrid baked+SD e-reader: ra_box grid layout + ra_ui geometry draw through
# ra_gfx into the SDRAM framebuffer ra_display_pal scans over GLCDC to the
# 1024x600 panel. ra_book + miniz inflate the compressed .rabook blobs (baked in
# MRAM via library.h, or read from SD). src/*.c hold the per-screen modules.
ra_add_app(NAME ereader_shelf STACK_BYTES 16384
           DESCRIPTION "Hybrid baked+SD e-reader: shelf, covers, TOC, full reader (1024x600 panel)"
           LIBS ra_gfx ra_display_pal ra_box ra_ui ra_book ra_fs ra_sdmmc_spi ra_epub ra_reflow)
