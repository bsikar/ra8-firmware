# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

cmake_minimum_required(VERSION 3.20)
project(glyph_bench LANGUAGES C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

get_filename_component(RA8_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
file(GLOB GLYPH_BENCH_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")

# ra8_glyph_atlas is Zig now, so the bench links the ra8_mem archive instead of
# compiling that file by path.
if(NOT TARGET ra8_zig::ra8_mem)
  include(${RA8_ROOT}/tests/cmake/zig_library.cmake)
  ra8_add_zig_library(
    NAME ra8_mem
    ZIG_ROOT ${RA8_ROOT}/libs/ra8_mem
    LIBRARY_NAME ra8_mem
  )
endif()

# The archive is a single object, so linking it brings along every extern in it:
# the four ra8_keycache_* the glyph atlas and the page cache are facades over.
# libs/ra8_mem is Zig apart from the arena, which this bench does not use, so
# there is nothing left to compile by path: the archive defines every ra8_mem
# symbol the bench references.
add_executable(
  glyph_bench
  ${GLYPH_BENCH_SOURCES}
  ${RA8_ROOT}/libs/ra8_io/src/ra8_io_stream.c
  ${RA8_ROOT}/port/posix/src/ra8_io_stream_posix.c
  ${RA8_ROOT}/libs/ra8_core/src/ra8_log.c
)
target_link_libraries(glyph_bench PRIVATE ra8_zig::ra8_mem)
target_include_directories(
  glyph_bench PRIVATE ${RA8_ROOT}/libs/ra8_core/inc ${RA8_ROOT}/libs/ra8_mem/inc
                      ${RA8_ROOT}/libs/ra8_io/inc ${RA8_ROOT}/port/posix/inc
)
target_compile_definitions(glyph_bench PRIVATE RA8_OFF_TARGET)
target_compile_options(
  glyph_bench
  PRIVATE -O2
          -g
          -Wall
          -Wextra
          -Werror
)
