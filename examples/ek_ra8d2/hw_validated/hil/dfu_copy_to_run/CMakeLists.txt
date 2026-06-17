#
# Per-app CMakeLists for dfu_copy_to_run (copy-to-run HIL proof).
#
# Embeds the copy-to-run demo payload (payload_image.h, generated from payload.c
# by build_payload.sh) and hands it to the shared libs/ra_dfu ra_dfu_launch --
# the same launcher the dfu_bootloader uses -- which copies it to the SRAM run
# base (k_ra_dfu_run_base) and branches there. The HIL alive gate then sees the
# PC in the SRAM run window, proving copy-to-run executed (issue #97).
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
ra_add_app(NAME dfu_copy_to_run STACK_BYTES 2200
           DESCRIPTION "Copy-to-run HIL proof: launch one embedded image at the SRAM run base via ra_dfu_launch"
           LIBS ra_dfu
          )
