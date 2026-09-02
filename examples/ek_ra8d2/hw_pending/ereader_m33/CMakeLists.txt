# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMake stub for `ereader_m33`. The Cortex-M85 (CPU0) image is built by
# the shared cmake/ra8_add_app.cmake recipe; the Cortex-M33 (CPU1) image is a
# second executable that runs the e-reader page-turn loop and is objcopy'd into
# the M85 ELF so one SWD flash drops both cores' images.
#
# Standalone:  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8d2.cmake
# Embedded:    pulled in by the top-level CMakeLists via add_subdirectory().
#
#
cmake_minimum_required(VERSION 3.20)

# A standalone `cmake -S . -B build` makes this file the top-level listfile,
# which CMake requires to contain a literal, direct project() call. The
# aggregate repo-root build instead pulls this app in via add_subdirectory()
# (project() is already called at the root), so guard on PARENT_DIRECTORY to
# avoid a redundant nested sub-project. ra8_add_app() below re-runs project() in
# the standalone case to apply the shared VERSION and the app description.
get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(ereader_m33 LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

ra8_add_app(
  NAME ereader_m33
  STACK_BYTES 2200 AUX_SRCS src/cpu1_main.c
  DESCRIPTION "Dual-core (M85 + M33) e-reader: M33 runs the page-turn loop while the M85 parks"
)

#############################################################################
# Second (Cortex-M33 / CPU1) image, embedded into the M85 ELF.
# The shared cmake/ra8_add_cpu1_image() helper builds cpu1_main.c as a
# freestanding M33 executable and links its .cpu1_image blob into the M85 ELF;
# linker_script.ld pins .cpu1_image at ORIGIN(MRAM_CPU1) (0x020C0000).
#
# The M33 image walks the baked RABOOK1 book through the header-only book.h
# inline accessors (pure offset arithmetic) and RENDERS the held page through the
# production ra8_gfx text stack. ra8_gfx is three dependency-clean, zero-heap,
# scalar (no-Helium) TUs whose only header dependency is ra8_core's ra8_err.h
# (already on the CPU1 include path), so it links cleanly into the freestanding
# M33 image -- no logging backend, no panel driver, no malloc. For the #150
# mode-switch it ALSO links the ra8_ipc send path (ra8_ipc.c) so the M33 can poke
# IPC0ISET0 to wake the parked M85 on a page turn; RA8_LOG_LEVEL=0 collapses
# ra8_ipc's ra8_log calls to no-ops so no logging backend is dragged in.
#############################################################################
ra8_add_cpu1_image(
  PARENT ereader_m33
  NAME ereader_m33_cpu1
  SOURCES src/cpu1_main.c
  INCLUDES ${RA8_REPO_ROOT}/apps/shared_libs/book/inc ${RA8_REPO_ROOT}/libs/ra8_gfx/inc
           ${RA8_REPO_ROOT}/libs/ra8_hal/inc
)

if(TARGET ereader_m33_cpu1.elf)
  target_sources(
    ereader_m33_cpu1.elf
    PRIVATE ${RA8_REPO_ROOT}/libs/ra8_gfx/src/ra8_gfx_text.c
            ${RA8_REPO_ROOT}/libs/ra8_gfx/src/ra8_gfx_text_glyph.c
            ${RA8_REPO_ROOT}/libs/ra8_gfx/src/ra8_gfx_font_8x16.c
            ${RA8_REPO_ROOT}/libs/ra8_hal/src/ra8_ipc.c
  )
  target_compile_definitions(ereader_m33_cpu1.elf PRIVATE RA8_LOG_LEVEL=0)
endif()
