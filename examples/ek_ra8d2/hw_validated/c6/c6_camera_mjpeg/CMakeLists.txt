# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

cmake_minimum_required(VERSION 3.20)

get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(c6_camera_mjpeg LANGUAGES C ASM)
endif()

set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

include("${_d}/examples/ek_ra8d2/common/c6_camera_server/c6_camera_server.cmake")
c6_camera_server_add_app(
  NAME
  c6_camera_mjpeg
  DESCRIPTION
  "OV5640 hardware-JPEG multipart stream over ESP32-C6 Wi-Fi"
  ROOT
  "${_d}"
)
