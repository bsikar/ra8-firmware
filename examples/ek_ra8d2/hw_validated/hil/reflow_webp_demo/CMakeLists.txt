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
  DESCRIPTION "Demonstrate inline WebP decoding in reflow text layout"
  LIBS ra8_gfx
       ra8_display_pal
       reflow
       webp
)

# The font baker moved from Python to Zig (font_to_c); it is built for the
# HOST with the pinned toolchain even in this Arm cross build, and a missing zig
# is a configure-time error rather than a silently skipped bake.
include("${_d}/cmake/ra8_zig_tool.cmake")
ra8_add_zig_host_tool(NAME font_to_c ZIG_ROOT "${_d}/tools/font_to_c")
set(_ra8_baked_font_c "${CMAKE_CURRENT_BINARY_DIR}/ra8_font_literata_latin1.c")
add_custom_command(
  OUTPUT "${_ra8_baked_font_c}"
  COMMAND
    $<TARGET_FILE:ra8_zig::font_to_c> "${_d}/libs/ra8_fonts/literata_latin1.ttf"
    "${_ra8_baked_font_c}" g_ra8_font_literata_latin1 literata_latin1.h
  # The font baker executable is a FILE input: a custom TARGET in DEPENDS is only
  # order-only under Ninja, so a rebuilt font_to_c must invalidate the baked array.
  DEPENDS "${_d}/libs/ra8_fonts/literata_latin1.ttf" "$<TARGET_FILE:ra8_zig::font_to_c>"
          ra8_zig_tool_font_to_c
  COMMENT "Generating baked Latin-1 font array (reflow_webp_demo)"
)
target_sources(reflow_webp_demo.elf PRIVATE "${_ra8_baked_font_c}")
target_include_directories(reflow_webp_demo.elf PRIVATE "${_d}/libs/ra8_fonts")
