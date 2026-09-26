# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMake stub for `widget_keyboard_demo`. All build logic lives in the
# shared cmake/ra8_add_app.cmake recipe; this file only names the app + its deps.
#
# Standalone:  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8d2.cmake
# Embedded:    pulled in by the top-level CMakeLists via add_subdirectory().
#
cmake_minimum_required(VERSION 3.20)

# A standalone `cmake -S . -B build` makes this file the top-level listfile,
# which CMake requires to contain a literal, direct project() call. The
# aggregate repo-root build instead pulls this app in via add_subdirectory()
# (project() is already called at the root), so guard on PARENT_DIRECTORY to
# avoid a redundant nested sub-project.
get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(widget_keyboard_demo LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

# ra8_widget carries no graphics dependency by design: the keyboard leaf draws
# through an injected ra8_widget_paint_t and reaches the key engine through an
# injected seam. This app binds the seam to ra8_keyboard (the real grid + text
# buffer) and the paint backend to a recording one, so the only libs it needs
# are the widget layer, the key engine, and the console stack.
#
# ra8_io globs every backend/sink (sd-spi -> ra8_sdmmc_spi, usb-cdc ->
# ra8_usb_pal), so those libs are listed to satisfy their includes, same as
# devcfg_record_demo.
ra8_add_app(
  NAME widget_keyboard_demo
  STACK_BYTES 4096
  DESCRIPTION "ra8_widget_keyboard: the on-screen-keyboard widget routed against the real ra8_keyboard engine"
  LIBS ra8_widget ra8_keyboard ra8_box ra8_ui ra8_display_pal ra8_gfx ra8_io ra8_fs ra8_sdmmc_spi ra8_usb_pal ra8_board_ek_ra8d2
)
