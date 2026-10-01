# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# ra8_board_ra8p1 -- board-support layer for the Renesas RA8P1 (R7KA8P1KFLCAC)
#
# This file documents the library's source / include layout. The per-app
# cross-compile build selects this layer via ra8_add_app(... BOARD ra8p1)
# (cmake/ra8_add_app.cmake), which globs src/*.c, adds inc/ to the include
# path, and falls back to src/boot/ + ld/ for any boot file / linker script the
# app does not override. Adding the directory here is sufficient for a
# standalone consumer to pick it up the same way the ek_ra8d2 layer is wired.
#
# The public LED / switch / console API is deliberately identical to
# ra8_board_ek_ra8d2 so an app is a pure board substitution (SOLID-L). Only one
# board layer is ever compiled into a given app, so the shared symbol names
# never collide. See issue RA8FW-260.
#
#

cmake_minimum_required(VERSION 3.20)

# The board implementation itself is Zig (build.zig, issue #2984): src/ holds
# no .c any more, so there is nothing to glob here. The objects arrive in the
# Zig static archive, which the per-app build registers on the board path in
# cmake/ra8_app/sources.cmake. src/boot/ stays C and is selected by
# ra8_add_app's boot fallback, not by this target.
if(NOT TARGET ra8_board_ra8p1)
  add_library(ra8_board_ra8p1 INTERFACE)
  target_include_directories(ra8_board_ra8p1 INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/inc)
endif()
