# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMake stub for `devcfg_record_demo`. All build logic lives in the
# shared cmake/ra8_add_app.cmake recipe; this file only names the app + its deps.
#
# Standalone:  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8d2.cmake
# Embedded:    pulled in by the top-level CMakeLists via add_subdirectory().
#
cmake_minimum_required(VERSION 3.20)

# A standalone `cmake -S . -B build` makes this file the top-level listfile,
# which CMake requires to contain a literal, direct project() call. The
# aggregate repo-root build instead pulls this app in via add_subdirectory()
# (project() is already called at the root), so guard on PARENT_DIRECTORY to
# avoid a redundant nested sub-project.
get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(devcfg_record_demo LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

# The demo binds an app-owned RAM medium through ra8_devcfg_store_t, commits a
# record, reloads it and compares field by field. The real extra-MRAM store is
# read-only (that window is one-time-programmable; see the README).
#
# ra8_io globs every backend/sink (sd-spi -> ra8_sdmmc_spi, usb-cdc ->
# ra8_usb_pal), so those libs are listed to satisfy their includes, same as
# ra8_io_mram_demo.
ra8_add_app(
  NAME devcfg_record_demo
  STACK_BYTES 4096
  DESCRIPTION "ra8_devcfg: RAM-store commit/resolve round trip plus a read-only unit probe"
  LIBS ra8_devcfg ra8_io ra8_fs ra8_sdmmc_spi ra8_usb_pal
)
