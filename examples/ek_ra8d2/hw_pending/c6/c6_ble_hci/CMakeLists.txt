# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMake stub for `c6_ble_hci`.

cmake_minimum_required(VERSION 3.20)

get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(c6_ble_hci LANGUAGES C ASM)
endif()

set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

ra8_add_app(
  NAME c6_ble_hci
  THREADX_HEAP SDRAM
  STACK_BYTES 8192
  DESCRIPTION "Exercise BLE HCI over ESP32-C6 SPI via ra8_ble and ble_hci_c6link"
  USES threadx esp_hosted
  LIBS ra8_board_ek_ra8d2 ra8_c6link
  EXTRA_SRCS "${_d}/port/nimble/src/ble_hci_c6link.c"
)

if(TARGET c6_ble_hci.elf)
  target_include_directories(c6_ble_hci.elf PRIVATE "${_d}/port/nimble/inc")
endif()
