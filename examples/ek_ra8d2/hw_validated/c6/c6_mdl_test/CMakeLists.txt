# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
cmake_minimum_required(VERSION 3.20)

get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(c6_mdl_test LANGUAGES C ASM)
endif()

set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

ra8_add_app(
  NAME c6_mdl_test
  STACK_BYTES 8192
  DESCRIPTION "Test media downloader RPC client"
  USES threadx esp_hosted
  LIBS ra8_board_ek_ra8d2 ra8_mem ra8_c6link
)
