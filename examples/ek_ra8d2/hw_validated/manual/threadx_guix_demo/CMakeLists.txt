#
# Per-app CMake stub for `threadx_guix_demo`. Build logic lives in the
# shared cmake/ra_add_app.cmake recipe; this file names the app + its deps.
#
# Standalone:  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8d2.cmake
# Embedded:    pulled in by the top-level CMakeLists via add_subdirectory().
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
cmake_minimum_required(VERSION 3.20)

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
    get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra_add_app.cmake")

ra_add_app(NAME threadx_guix_demo STACK_BYTES 4400
           DESCRIPTION "ThreadX + GUIX hello-world demo for RA8D2"
           USES threadx guix
           LIBS ra_gfx ra_display_pal)

# GUIX additionally needs the board GLCDC port headers and the RA_USE_*
# feature gates compiled into the app's own translation units.
target_include_directories(threadx_guix_demo.elf PRIVATE ${RA_REPO_ROOT}/port/guix)
target_compile_definitions(threadx_guix_demo.elf PRIVATE RA_USE_THREADX RA_USE_GUIX)

# GUIX font resources live flat under libs/fonts/ (not libs/<x>/src).
file(GLOB_RECURSE _ra_guix_fonts CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/fonts/*.c)
target_sources(threadx_guix_demo.elf PRIVATE ${_ra_guix_fonts})
