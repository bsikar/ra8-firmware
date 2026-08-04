# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMakeLists for dfu_selftest_fs_host (USB self-loop Config B).
#
# FS host (J11) DFU-programs the inactive MRAM slot on the HS DFU device (J7),
# then DFU_UPLOADs it back and byte-verifies. One image runs both roles over the
# J7<->J11 cable. Uses the controller-agnostic libs/ra8_dfu core.
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
  project(dfu_selftest_fs_host LANGUAGES C ASM)
endif()
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")
ra8_add_app(
  NAME dfu_selftest_fs_host
  STACK_BYTES 4000
  DESCRIPTION "USB self-loop B: FS host DFU-programs the inactive MRAM slot, verifies"
  USES threadx usbx
  LIBS ra8_dfu ra8_board_ek_ra8d2
)
