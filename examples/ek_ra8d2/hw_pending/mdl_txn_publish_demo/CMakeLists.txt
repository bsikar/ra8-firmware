# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMake stub for `mdl_txn_publish_demo`. All build logic lives in the
# shared cmake/ra8_add_app.cmake recipe; this file only names the app + deps.
#
# Standalone:  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8d2.cmake
# Embedded:    pulled in by the top-level CMakeLists via add_subdirectory().
#
#
cmake_minimum_required(VERSION 3.20)

get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(mdl_txn_publish_demo LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

# `if` + `if_ra8_vfs` supply the portable facade and the VFS binding, and
# `mdl_storage_txn` is the adapter under test (the recipe adds ra8_c6link's
# headers for the seam typedef; no C6 code is compiled or opened here).
# ra8_io globs every backend/sink, so
# ra8_sdmmc_spi / ra8_usb_pal are listed only to satisfy those includes; this
# app's medium is a RAM blockdev it owns. (#762)
ra8_add_app(
  NAME mdl_txn_publish_demo
  STACK_BYTES 4096
  DESCRIPTION "mdl_storage_txn: C6 media storage seam on the fw_if_fs transaction port"
  LIBS if
       if_ra8_vfs
       mdl_storage_txn
       ra8_io
       ra8_fs
       ra8_sdmmc_spi
       ra8_usb_pal
)
