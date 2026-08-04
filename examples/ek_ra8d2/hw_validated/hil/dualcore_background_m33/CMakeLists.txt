# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMake stub for `dualcore_background_m33`. The Cortex-M85 (CPU0)
# image is built by the shared cmake/ra8_add_app.cmake recipe; the Cortex-M33
# (CPU1) image is a second executable that is objcopy'd into the M85 ELF so
# one SWD flash drops both cores' images.
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
  project(dualcore_background_m33 LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

ra8_add_app(
  NAME dualcore_background_m33
  STACK_BYTES 2200
  DESCRIPTION
    "Dual-core (M85 + M33) autonomous counter demo: M33 runs independently while M85 yields"
)

#############################################################################
# Second (Cortex-M33 / CPU1) image, embedded into the M85 ELF.
# The shared cmake/ra8_add_cpu1_image() helper builds cpu1_main.c as a
# freestanding M33 executable and links its .cpu1_image blob into the M85 ELF;
# linker_script.ld pins .cpu1_image at ORIGIN(MRAM_CPU1) (0x020C0000).
#############################################################################
ra8_add_cpu1_image(
  PARENT dualcore_background_m33
  NAME dualcore_background_m33_cpu1
  SOURCES cpu1_main.c
)
