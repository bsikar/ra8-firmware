#
# Per-app CMake stub for `threadx_filex_levelx_demo`. Build logic lives in
# the shared cmake/ra8_add_app.cmake recipe; this file names the app + deps.
#
# Standalone:  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8d2.cmake
# Embedded:    pulled in by the top-level CMakeLists via add_subdirectory().
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
cmake_minimum_required(VERSION 3.20)

# A standalone `cmake -S . -B build` makes this file the top-level
# listfile, which CMake requires to contain a literal, direct project()
# call. The aggregate repo-root build instead pulls this app in via
# add_subdirectory() (project() is already called at the root), so guard on
# PARENT_DIRECTORY to avoid a redundant nested sub-project. ra8_add_app()
# below re-runs project() in the standalone case to apply the shared VERSION
# and the app description.
get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(threadx_filex_levelx_demo LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

# This demo pairs FileX with LevelX-on-xSPI flash (no SD/MMC), so it links
# the FileX *core* plus the LevelX flash port -- not the FileX SDHI port.
# ra8_add_app's USES covers ThreadX + LevelX; FileX is enabled + guarded
# here so its core can be linked without dragging in the SDHI bridge.
get_directory_property(_ra8_p PARENT_DIRECTORY)
if(NOT _ra8_p)
  option(RA8_USE_FILEX "Enable the vendored FileX FAT file system" ON)
  include("${_d}/cmake/filex.cmake")
endif()
if(NOT RA8_USE_FILEX)
  message(STATUS "threadx_filex_levelx_demo: skipped (RA8_USE_FILEX is OFF)")
  return()
endif()

ra8_add_app(
  NAME threadx_filex_levelx_demo
  STACK_BYTES 2200
  DESCRIPTION "ThreadX + FileX-on-LevelX wear-levelled FAT demo for RA8D2"
  LIBS ra8_board_ek_ra8d2
  USES threadx levelx
)

target_link_libraries(threadx_filex_levelx_demo.elf PRIVATE filex)
get_property(_ra8_lvfx GLOBAL PROPERTY RA8_LEVELX_FILEX_PORT_SOURCES)
if(_ra8_lvfx)
  target_sources(threadx_filex_levelx_demo.elf PRIVATE ${_ra8_lvfx})
endif()
