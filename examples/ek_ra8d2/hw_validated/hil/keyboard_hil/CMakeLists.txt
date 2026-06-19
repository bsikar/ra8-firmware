#
# Per-app CMake stub for `keyboard_hil`. The on-screen keyboard widget
# (ra_keyboard) + the hit-test/rect toolkit it builds on (ra_ui) are pulled
# via LIBS; all build logic lives in cmake/ra_add_app.cmake.
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

ra_add_app(NAME keyboard_hil STACK_BYTES 3072
           DESCRIPTION "keyboard_hil firmware for RA8D2"
           LIBS ra_keyboard ra_ui)
