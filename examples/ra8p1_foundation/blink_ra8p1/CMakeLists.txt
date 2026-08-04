# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMake stub for `blink_ra8p1` -- the RA8P1 build-foundation app.
#
# Build it STANDALONE with the RA8P1 toolchain (it is deliberately excluded from
# the top-level RA8D2 unified build so `make blink` / `make blink_hal` stay
# byte-for-behaviour unchanged -- see the "ra8p1_foundation" skip in the repo
# root CMakeLists.txt):
#
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8p1.cmake
#   cmake --build build
#
# or simply `make` in this directory (its Makefile points at toolchain-ra8p1).
#
# All build logic lives in the shared cmake/ra8_add_app.cmake recipe; this file
# only names the app. BOARD ra8p1 selects the dedicated libs/ra8_board_ra8p1
# layer (issue #226); the recipe compiles the full ra8_core + ra8_hal + that
# board layer, so this app doubles as the "does the foundation compile+link for
# R7KA8P1KFLCAC" proof.
#
#
cmake_minimum_required(VERSION 3.20)

get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(blink_ra8p1 LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

ra8_add_app(
  NAME blink_ra8p1
  BOARD ra8p1
  STACK_BYTES 2200
  DESCRIPTION "RA8P1 build-foundation blink (R7KA8P1KFLCAC)"
)
