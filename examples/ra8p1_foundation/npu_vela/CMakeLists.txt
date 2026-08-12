# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMake stub for `npu_vela` -- the RA8P1 Ethos-U55 .npub container loader
# end-to-end demo (on the way to issue #227).
#
# Build it STANDALONE with the RA8P1 toolchain (it is deliberately excluded from
# the top-level RA8D2 unified build so the RA8D2 build never touches an NPU app --
# see the "ra8p1_foundation" skip in the repo root CMakeLists.txt):
#
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8p1.cmake
#   cmake --build build
#
# or simply `make` in this directory (its Makefile points at toolchain-ra8p1).
#
# The shared cmake/ra8_add_app.cmake recipe compiles the full ra8_core + ra8_hal
# (including the RA8P1-only ra8_npu.c + ra8_npu_loader.c, live under
# -DRA8_DEVICE_RA8P1) + the reused ra8_board_ek_ra8d2 layer. This app additionally
# includes the committed, generated model header under tools/vela/generated/, so
# the include path is extended to that directory after the app is defined.
#
#
cmake_minimum_required(VERSION 3.20)

get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(npu_vela LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

ra8_add_app(
  NAME npu_vela
  STACK_BYTES 2400
  DESCRIPTION "RA8P1 Ethos-U55 .npub container loader end-to-end (R7KA8P1KFLCAC)"
)

# The generated golden model container header lives in tools/vela/generated/.
target_include_directories(npu_vela.elf PRIVATE "${_d}/tools/vela/generated")
