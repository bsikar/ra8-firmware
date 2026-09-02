# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMake stub for `ereader_rabook`. All build logic lives in the
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
# below re-runs project() in the standalone case to apply the shared VERSION
# and the app description. CXX remains enabled for linked C++ components;
# the project therefore keeps CXX enabled.

get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(ereader_rabook LANGUAGES C CXX ASM)
endif()

set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

# book provides the compiled-book loader + the DOM->XHTML render bridge;
# reflow lays it out and ra8_gfx rasterizes. No epub / miniz: the fixture
# is baked inflated, so the app validates the flat blob directly.
ra8_add_app(
  NAME ereader_rabook
  STACK_BYTES 16384
  DESCRIPTION "Headless load + render of a compiled .rabook, small to large (HIL gate)"
  LIBS ra8_gfx reflow book ra8_board_ek_ra8d2
)
