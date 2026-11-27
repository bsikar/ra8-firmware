# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMakeLists for usb_role_flip (USB runtime role flip self-loop).
#

cmake_minimum_required(VERSION 3.20)

get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(usb_role_flip LANGUAGES C ASM)
endif()

set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

ra8_add_app(
  NAME usb_role_flip
  THREADX_HEAP SDRAM
  STACK_BYTES 4000
  DESCRIPTION "USB runtime role flip: swap HS and FS roles at runtime via DCD/HCD/PAL deinit"
  USES threadx usbx
  LIBS ra8_board_ek_ra8d2 ra8_usb_pal
)
