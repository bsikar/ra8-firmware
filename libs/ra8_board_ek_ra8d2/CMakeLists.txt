# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# ra8_board_ek_ra8d2 -- board-support layer for the Renesas EK-RA8D2 v1
#
# This file documents the library's source / include layout. The
# top-level cross-compile build (examples/<app>/CMakeLists.txt) and
# the host-compiled test build (tests/CMakeLists.txt) discover the
# .c / .h files via file(GLOB_RECURSE ...) the same way they do for
# the rest of the libs/ tree -- so adding the directory here is
# sufficient for it to be picked up automatically.
#
# Each example app that wants the BSP must add the include directory
# below and link in the source file. See examples/blink/CMakeLists.txt
# for the canonical wiring.
#
#

cmake_minimum_required(VERSION 3.20)

if(NOT TARGET ra8_board_ek_ra8d2)
  add_library(ra8_board_ek_ra8d2 INTERFACE)
  target_include_directories(ra8_board_ek_ra8d2 INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/inc)
  file(GLOB RA8_BOARD_EK_RA8D2_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
  target_sources(ra8_board_ek_ra8d2 INTERFACE ${RA8_BOARD_EK_RA8D2_SOURCES})
endif()
