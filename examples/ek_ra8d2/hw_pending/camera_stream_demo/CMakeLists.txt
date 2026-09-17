# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMake entry point for `camera_stream_demo`. All build logic lives in the
# shared cmake/ra8_add_app.cmake recipe; this file only names the app and its deps.
#
# Standalone:  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8d2.cmake
# Embedded:    pulled in by the top-level CMakeLists via add_subdirectory().
#
cmake_minimum_required(VERSION 3.20)

# A standalone `cmake -S . -B build` makes this file the top-level listfile,
# which CMake requires to contain a literal, direct project() call. The
# aggregate repo-root build instead pulls this app in via add_subdirectory().
get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(camera_stream_demo LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

# This is the first app that links the optional `ra8_camera_io` bridge: it is a
# separate library from `ra8_camera` so capture-only consumers do not inherit
# `ra8_io`, which means it needs an app that opts into both to be built at all.
ra8_add_app(
  NAME camera_stream_demo
  STACK_BYTES 4096
  DESCRIPTION "ra8_camera_io bridge: encode one frame straight into an ra8_io stream sink"
  LIBS ra8_camera ra8_camera_io ra8_io
)
