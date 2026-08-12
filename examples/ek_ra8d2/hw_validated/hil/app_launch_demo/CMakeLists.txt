# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMake stub for `app_launch_demo`. All build logic lives in the
# shared cmake/ra8_add_app.cmake recipe; this file only names the app + its deps.
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
  project(app_launch_demo LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

# Build-time exclusion of the optional "settings" app (#146 "core uninstallable").
# Default 1 (settings shipped); configure with -DAPP_LAUNCH_SETTINGS=0 to drop it
# from the registry. Forwarded as a compile definition so the C `#if` sees it;
# applies to the elf target ra8_add_app() creates below (directory scope).
set(APP_LAUNCH_SETTINGS
    1
    CACHE STRING "Ship the optional settings app (1) or exclude it (0)"
)
add_compile_definitions(APP_LAUNCH_SETTINGS=${APP_LAUNCH_SETTINGS})

ra8_add_app(
  NAME app_launch_demo
  STACK_BYTES 4096
  DESCRIPTION "ra8_app registry + launch + back-stack chrome stub, headless on ITM (#146)"
  LIBS ra8_app ra8_widget ra8_box ra8_ui
)
