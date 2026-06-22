#
# Per-app CMakeLists for dfu_bootloader (immutable USB-DFU MRAM bootloader).
#
# The first 128 KiB of code-MRAM. At reset it boots the valid application slot
# (software A/B select + jump) or, when no slot is valid (or a trigger is set),
# brings up the USBX DFU device on USB-FS and programs the inactive slot. Uses
# the controller-agnostic libs/ra_dfu core.
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
    project(dfu_bootloader LANGUAGES C ASM)
endif()
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
    get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra_add_app.cmake")
ra_add_app(NAME dfu_bootloader STACK_BYTES 4000
           DESCRIPTION "Immutable USB-DFU MRAM bootloader: A/B slot boot + jump, or DFU the inactive slot over USB-FS"
           USES threadx usbx
           LIBS ra_dfu
          )
