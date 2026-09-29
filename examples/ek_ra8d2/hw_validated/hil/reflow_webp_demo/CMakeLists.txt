# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

cmake_minimum_required(VERSION 3.20)

get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(reflow_webp_demo LANGUAGES C ASM)
endif()

set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

ra8_add_app(
  NAME reflow_webp_demo
  STACK_BYTES 8192
  DESCRIPTION "Demonstrate inline WebP decoding in reflow text layout (#637)"
  LIBS ra8_gfx
       ra8_display_pal
       reflow
       webp
)

set(_ra8_baked_font_c "${CMAKE_CURRENT_BINARY_DIR}/ra8_font_literata_latin1.c")
add_custom_command(
  OUTPUT "${_ra8_baked_font_c}"
  COMMAND
    ${CMAKE_COMMAND} -E env python3 "${_d}/scripts/gen/font_to_c.py"
    "${_d}/libs/ra8_fonts/literata_latin1.ttf" "${_ra8_baked_font_c}" g_ra8_font_literata_latin1
    literata_latin1.h
  DEPENDS "${_d}/libs/ra8_fonts/literata_latin1.ttf" "${_d}/scripts/gen/font_to_c.py"
  COMMENT "Generating baked Latin-1 font array (reflow_webp_demo)"
)
target_sources(reflow_webp_demo.elf PRIVATE "${_ra8_baked_font_c}")
target_include_directories(reflow_webp_demo.elf PRIVATE "${_d}/libs/ra8_fonts")
