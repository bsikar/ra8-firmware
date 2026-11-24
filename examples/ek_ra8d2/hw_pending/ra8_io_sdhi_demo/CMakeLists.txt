# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMake stub for `ra8_io_sdhi_demo`. All build logic lives in the shared
# cmake/ra8_add_app.cmake recipe; this file only names the app + its deps.
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
  project(ra8_io_sdhi_demo LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

ra8_add_app(
  NAME ra8_io_sdhi_demo
  STACK_BYTES 4096
  DESCRIPTION "ra8_io swappable native-SDHI backend: VFS round-trip on microSD"
  LIBS ra8_io
       ra8_fs
       ra8_sdmmc_spi
       ra8_usb_pal
       ra8_board_ek_ra8d2
)

if(NOT RA8_ZIG_EXECUTABLE)
  message(FATAL_ERROR "ra8_io_sdhi_demo requires zig on PATH to compile src/sdhi_demo.zig")
endif()

_ra8_zig_target_for_toolchain(_sdhi_zig_target _sdhi_zig_cpu)
set(_sdhi_zig_src "${CMAKE_CURRENT_SOURCE_DIR}/src/sdhi_demo.zig")
set(_sdhi_zig_obj "${CMAKE_CURRENT_BINARY_DIR}/zig/sdhi_demo.o")
add_custom_command(
  OUTPUT "${_sdhi_zig_obj}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/zig"
  COMMAND
    ${CMAKE_COMMAND} -E env "${RA8_ZIG_EXECUTABLE}" build-obj -target ${_sdhi_zig_target}
    -mcpu ${_sdhi_zig_cpu} -O ${RA8_ZIG_OPTIMIZE} -fno-unwind-tables -fno-compiler-rt
    "${_sdhi_zig_src}" --cache-dir "${CMAKE_CURRENT_BINARY_DIR}/zig/.cache"
    "-femit-bin=${_sdhi_zig_obj}"
  DEPENDS "${_sdhi_zig_src}"
  COMMENT "Building Zig module sdhi_demo.zig for ${_sdhi_zig_target} ${_sdhi_zig_cpu}"
  VERBATIM
)
set_source_files_properties("${_sdhi_zig_obj}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
target_sources(ra8_io_sdhi_demo.elf PRIVATE "${_sdhi_zig_obj}")
