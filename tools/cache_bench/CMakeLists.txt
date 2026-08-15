# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# tools/cache_bench -- host build of the #147 eviction benchmark and the #208
# block/frame-size sweep.
#
# The day-to-day build is the plain Makefile next to this file (`make` /
# `make sweep`); CI's `make bench-cache` also drives the Makefile. This CMake
# project exists so the tool has a compile_commands.json for the clang-tidy
# gate (scripts/checks/clang_tidy.sh runs a tools pass against a configured build
# tree) and stays byte-equivalent to the Makefile: the same source glob, the
# same firmware TUs compiled in directly (the sweep drives the REAL
# ra8_vmem/ra8_vsource page cache and the REAL ra8_book_chunked RBKC reader),
# and miniz (SOUP) built with -w -fno-strict-aliasing exactly as
# tests/CMakeLists.txt compiles it (gcc miscompiles its inflate under strict
# aliasing at -O2).
#
#   cmake -B build -S . && cmake --build build
#
#

cmake_minimum_required(VERSION 3.20)
project(ra8_cache_bench LANGUAGES C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(RA8_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

# Every first-party .c in this directory is part of the one cache_bench
# binary (mirrors the Makefile's `$(wildcard *.c)`), so new TUs need no edit
# here or there.
file(GLOB CACHE_BENCH_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.c")

add_executable(
  cache_bench
  ${CACHE_BENCH_SOURCES}
  ${RA8_ROOT}/libs/ra8_mem/src/ra8_vmem.c
  ${RA8_ROOT}/libs/ra8_mem/src/ra8_keycache.c
  ${RA8_ROOT}/libs/ra8_mem/src/ra8_vsource.c
  ${RA8_ROOT}/libs/ra8_book/src/ra8_book.c
  ${RA8_ROOT}/libs/ra8_book/src/ra8_book_chunked.c
  ${RA8_ROOT}/libs/third_party/miniz/miniz.c
)

target_include_directories(
  cache_bench PRIVATE ${RA8_ROOT}/libs/ra8_core/inc ${RA8_ROOT}/libs/ra8_mem/inc
                      ${RA8_ROOT}/libs/ra8_book/inc ${RA8_ROOT}/libs/third_party/miniz
)

target_compile_options(
  cache_bench
  PRIVATE -O2
          -g
          -Wall
          -Wextra
          -Werror
          -Wno-unused-parameter
)

target_compile_definitions(cache_bench PRIVATE MINIZ_NO_MALLOC MINIZ_NO_STDIO)

# miniz is SOUP: silence its warnings and defeat the strict-aliasing
# miscompile, matching the Makefile's dedicated miniz_host.o rule.
set_source_files_properties(
  ${RA8_ROOT}/libs/third_party/miniz/miniz.c PROPERTIES COMPILE_OPTIONS "-w;-fno-strict-aliasing"
)

include(CTest)
if(BUILD_TESTING)
  add_executable(
    test_cache_bench
    tests/test_cache_bench.c
    cache_bench.c
    cache_bench_host.c
    cache_bench_io.c
    policies.c
    policy_scanresist.c
    sweep_block.c
    sweep_block_backends.c
    sweep_block_payload.c
    sweep_block_report.c
    trace.c
    ${RA8_ROOT}/libs/ra8_mem/src/ra8_vmem.c
    ${RA8_ROOT}/libs/ra8_mem/src/ra8_keycache.c
    ${RA8_ROOT}/libs/ra8_mem/src/ra8_vsource.c
    ${RA8_ROOT}/libs/ra8_book/src/ra8_book.c
    ${RA8_ROOT}/libs/ra8_book/src/ra8_book_chunked.c
    ${RA8_ROOT}/libs/third_party/miniz/miniz.c
  )
  target_include_directories(
    test_cache_bench
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
            ${RA8_ROOT}/libs/ra8_core/inc
            ${RA8_ROOT}/libs/ra8_mem/inc
            ${RA8_ROOT}/libs/ra8_book/inc
            ${RA8_ROOT}/libs/third_party/miniz
  )
  target_compile_definitions(
    test_cache_bench PRIVATE CB_CACHE_BENCH_LIBRARY_ONLY MINIZ_NO_MALLOC MINIZ_NO_STDIO
  )
  target_compile_options(
    test_cache_bench
    PRIVATE -O1
            -g
            -Wall
            -Wextra
            -Werror
            -Wno-unused-parameter
  )
  add_test(NAME cache_bench_contract COMMAND test_cache_bench)
endif()
