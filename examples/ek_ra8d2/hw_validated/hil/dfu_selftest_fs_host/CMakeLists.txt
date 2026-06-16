#
# Per-app CMakeLists for dfu_selftest_fs_host (USB self-loop Config B).
#
# FS host (J11) DFU-programs the inactive MRAM slot on the HS DFU device (J7),
# then DFU_UPLOADs it back and byte-verifies. One image runs both roles over the
# J7<->J11 cable. Uses the controller-agnostic libs/ra_dfu core.
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
ra_add_app(NAME dfu_selftest_fs_host STACK_BYTES 4000
           DESCRIPTION "USB self-loop Config B: FS host DFU-programs the inactive MRAM slot on an HS DFU device, verifies"
           USES threadx usbx
           LIBS ra_dfu
          )
