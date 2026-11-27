# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMake stub for `translate_eval_e2e`. All build logic lives in the
# shared cmake/ra8_add_app.cmake recipe; this file only names the app + deps.
#
# Standalone:  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8d2.cmake
# Embedded:    pulled in by the top-level CMakeLists via add_subdirectory().

cmake_minimum_required(VERSION 3.20)

get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(translate_eval_e2e LANGUAGES C CXX ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

ra8_add_app(
  NAME translate_eval_e2e
  STACK_BYTES 32768
  DESCRIPTION "End-to-end manga translation E2E runner: TPAK SD harness, chapter job, and TRJC resume"
  LIBS ra8_translate
       ra8_ui
       ra8_widget
       ra8_box
       ra8_gfx
       ra8_fs
       ra8_board_ek_ra8d2
)
