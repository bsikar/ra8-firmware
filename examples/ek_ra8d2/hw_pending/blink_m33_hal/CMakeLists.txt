# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMake stub for `blink_m33_hal` -- the HAL-based twin of `blink_m33`.
#
# The Cortex-M85 (CPU0) launcher image is built by the shared
# cmake/ra8_add_app.cmake recipe; the Cortex-M33 (CPU1) blink image is built +
# embedded by ra8_add_cpu1_image(), so one SWD flash drops both cores' images.
# The only difference from `blink_m33` is that the CPU1 image drives LED1 through
# the header-only HAL primitive ra8_pcntr_set_output() (ra8_pcntr.h) instead of a
# raw PCNTR poke; ra8_add_cpu1_image() puts libs/ra8_hal/inc on the M33 include
# path so that freestanding-clean header resolves without linking any ra8_hal
# object (issue #580). Kept ALONGSIDE the raw `blink_m33`, which stays the
# deliberate raw-MMIO reference.
#
# Standalone:  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8d2.cmake
# Embedded:    pulled in by the top-level CMakeLists via add_subdirectory().
#
#
cmake_minimum_required(VERSION 3.20)

# A standalone `cmake -S . -B build` makes this file the top-level listfile,
# which CMake requires to contain a literal, direct project() call. The aggregate
# repo-root build instead pulls this app in via add_subdirectory() (project() is
# already called at the root), so guard on PARENT_DIRECTORY to avoid a redundant
# nested sub-project. ra8_add_app() below re-runs project() in the standalone case
# to apply the shared VERSION and the app description.
get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(blink_m33_hal LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

ra8_add_app(
  NAME blink_m33_hal
  STACK_BYTES 2200 AUX_SRCS src/cpu1_main.c
  DESCRIPTION "Cortex-M33 secondary-core LED blink via the HAL PCNTR primitive for RA8D2"
)

#############################################################################
# Second (Cortex-M33 / CPU1) image, embedded into the M85 ELF.
# ra8_add_cpu1_image() builds cpu1_main.c as a freestanding M33 executable and
# links its .cpu1_image blob into the M85 ELF; linker_script.ld pins .cpu1_image
# at ORIGIN(MRAM_CPU1) (0x020C0000). The recipe puts libs/ra8_hal/inc on the M33
# include path, so cpu1_main.c can include ra8_pcntr.h (header-only) with nothing
# linked from ra8_hal.
#############################################################################
ra8_add_cpu1_image(
  PARENT blink_m33_hal
  NAME blink_m33_hal_cpu1
  SOURCES src/cpu1_main.c
)
