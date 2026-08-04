# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMakeLists for dfu_copy_to_run (copy-to-run HIL proof).
#
# Embeds the copy-to-run demo payload (payload_image.h, generated from payload.c
# by build_payload.sh) and hands it to the shared libs/ra8_dfu ra8_dfu_launch --
# the same launcher the dfu_bootloader uses -- which copies it to the SRAM run
# base (k_ra8_dfu_run_base) and branches there. The HIL alive gate then sees the
# PC in the SRAM run window, proving copy-to-run executed (issue #97).
#
#
cmake_minimum_required(VERSION 3.20)

# A standalone `cmake -S . -B build` makes this file the top-level
# listfile, which CMake requires to contain a literal, direct project()
# call. The aggregate repo-root build instead pulls this app in via
# add_subdirectory() (project() is already called at the root), so guard on
# PARENT_DIRECTORY to avoid a redundant nested sub-project. ra8_add_app()
# below re-runs project() in the standalone case to apply the shared VERSION
# and the app description.
get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(dfu_copy_to_run LANGUAGES C ASM)
endif()
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")
ra8_add_app(
  NAME dfu_copy_to_run
  STACK_BYTES 2200
  DESCRIPTION
    "Copy-to-run HIL proof: launch one embedded image at the SRAM run base via ra8_dfu_launch"
  LIBS ra8_dfu
)
