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

add_executable(
  glyph_bench
  ${GLYPH_BENCH_SOURCES}
  ${RA8_ROOT}/libs/ra8_mem/src/ra8_glyph_atlas.c
  ${RA8_ROOT}/libs/ra8_mem/src/ra8_keycache.c
  ${RA8_ROOT}/libs/ra8_io/src/ra8_io_stream.c
  ${RA8_ROOT}/port/posix/src/ra8_io_stream_posix.c
  ${RA8_ROOT}/libs/ra8_core/src/ra8_log.c
)
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
