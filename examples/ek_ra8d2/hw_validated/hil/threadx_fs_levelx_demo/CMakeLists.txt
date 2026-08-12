# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMake stub for `threadx_fs_levelx_demo`. Build logic lives in
# the shared cmake/ra8_add_app.cmake recipe; this file names the app + deps.
#
# Standalone:  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8d2.cmake
# Embedded:    pulled in by the top-level CMakeLists via add_subdirectory().
#
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
  project(threadx_fs_levelx_demo LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

# This demo pairs ra8_fs with LevelX-on-xSPI flash (no SD/MMC): ra8_add_app's
# USES covers ThreadX + LevelX, LIBS pulls the first-party filesystem, and the
# LevelX -> ra8_fs block-device backend (port/levelx/src/lx_fs_backend.c) is
# spliced in below.
ra8_add_app(
  NAME threadx_fs_levelx_demo
  STACK_BYTES 2200
  DESCRIPTION "ThreadX + ra8_fs-on-LevelX wear-levelled FAT demo for RA8D2"
  LIBS ra8_board_ek_ra8d2 ra8_fs
  USES threadx levelx
)

get_property(_ra8_lvfs GLOBAL PROPERTY RA8_LEVELX_FS_PORT_SOURCES)
if(_ra8_lvfs)
  target_sources(threadx_fs_levelx_demo.elf PRIVATE ${_ra8_lvfs})
endif()
