#
# Per-app CMake stub for `compile_on_m33`. The Cortex-M85 (CPU0) image is built
# by the shared cmake/ra_add_app.cmake recipe and links ra_book so it can run
# ra_book_validate on the blob the M33 emits. The Cortex-M33 (CPU1) image is a
# second executable that links the zero-heap RABOOK1 emitter (ra_rabook_compile)
# and is objcopy'd into the M85 ELF so one SWD flash drops both cores' images.
#
# Standalone:  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8d2.cmake
# Embedded:    pulled in by the top-level CMakeLists via add_subdirectory().
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
cmake_minimum_required(VERSION 3.20)

# A standalone `cmake -S . -B build` makes this file the top-level listfile,
# which CMake requires to contain a literal, direct project() call. The
# aggregate repo-root build instead pulls this app in via add_subdirectory()
# (project() is already called at the root), so guard on PARENT_DIRECTORY to
# avoid a redundant nested sub-project. ra_add_app() below re-runs project() in
# the standalone case to apply the shared VERSION and the app description.
get_directory_property(_ra_has_parent PARENT_DIRECTORY)
if(NOT _ra_has_parent)
    project(compile_on_m33 LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
    get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra_add_app.cmake")

ra_add_app(NAME compile_on_m33 STACK_BYTES 4096
           DESCRIPTION "Dual-core (M85 + M33): the M33 runs the RABOOK1 emitter back-end; the M85 validates the blob (#149b)")

# The M85 image needs only ra_book_validate plus the header-only ra_book.h inline
# accessors, so link the single dependency-clean ra_book.c core directly -- NOT
# the whole ra_book lib via LIBS. ra_book's xhtml / paged TUs carry deep stack
# frames (ra_book_walk_to_xhtml ~4 KiB) and pull in ra_mem, none of which this
# validate-only app exercises; globbing them in would trip -Wstack-usage.
target_sources(compile_on_m33.elf PRIVATE ${RA_REPO_ROOT}/libs/ra_book/src/ra_book.c)
target_include_directories(compile_on_m33.elf PRIVATE ${RA_REPO_ROOT}/libs/ra_book/inc)

#############################################################################
# Second (Cortex-M33 / CPU1) image, embedded into the M85 ELF.
# The shared cmake/ra_add_cpu1_image() helper builds cpu1_main.c as a
# freestanding M33 executable and links its .cpu1_image blob into the M85 ELF;
# linker_script.ld pins .cpu1_image at ORIGIN(MRAM_CPU1) (0x020C0000).
#
# Unlike the header-only ereader_m33 reader, this image actually LINKS the
# zero-heap RABOOK1 emitter (libs/ra_rabook_compile/src/ra_rabook_compile.c),
# so it needs ra_book's and ra_rabook_compile's include dirs. The emitter only
# leans on memcpy/memset/strlen (newlib) plus ra_log/ra_check macros; defining
# RA_LOG_LEVEL=0 collapses every ra_log call to a no-op so no logging backend is
# dragged into the second core (it would otherwise resolve to internal_ra_log_*).
#############################################################################
ra_add_cpu1_image(
    PARENT   compile_on_m33
    NAME     compile_on_m33_cpu1
    SOURCES  cpu1_main.c
    INCLUDES ${RA_REPO_ROOT}/libs/ra_book/inc
             ${RA_REPO_ROOT}/libs/ra_rabook_compile/inc)

# ra_add_cpu1_image is a no-op on the host (no cross toolchain), so only wire the
# emitter source + log gate when the CPU1 target actually exists.
if(TARGET compile_on_m33_cpu1.elf)
    target_sources(compile_on_m33_cpu1.elf PRIVATE
        ${RA_REPO_ROOT}/libs/ra_rabook_compile/src/ra_rabook_compile.c)
    target_compile_definitions(compile_on_m33_cpu1.elf PRIVATE RA_LOG_LEVEL=0)
endif()
