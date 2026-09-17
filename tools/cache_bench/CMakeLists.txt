# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# tools/cache_bench -- host build of the #147 eviction benchmark and the #208
# block/frame-size sweep.
#
# The day-to-day build is `just tools::build_cache_bench`; the full sweep is
# `just tools::bench_cache`. This CMake project exists so the tool has a
# compile_commands.json for the clang-tidy
# gate (scripts/checks/clang_tidy.sh runs a tools pass against a configured build
# tree) and stays byte-equivalent to the justfile: the same source glob, the
# same firmware TUs compiled in directly (the sweep drives the REAL
# ra8_vmem/ra8_vsource page cache and the REAL book_chunked RBKC reader),
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
# binary (mirrors the authoritative tools recipe's source set), so new TUs need
# no edit here or there.
file(GLOB CACHE_BENCH_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")

add_executable(
  cache_bench
  ${CACHE_BENCH_SOURCES}
  ${RA8_ROOT}/libs/ra8_mem/src/ra8_vmem.c
  ${RA8_ROOT}/libs/ra8_mem/src/ra8_keycache.c
  ${RA8_ROOT}/libs/ra8_mem/src/ra8_vsource.c
  ${RA8_ROOT}/apps/shared_libs/book/src/book.c
  ${RA8_ROOT}/apps/shared_libs/book/src/book_chunked.c
  ${RA8_ROOT}/apps/shared_libs/third_party/miniz/miniz.c
)

target_include_directories(
  cache_bench
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc
          ${RA8_ROOT}/libs/ra8_core/inc
          ${RA8_ROOT}/libs/ra8_mem/inc
          ${RA8_ROOT}/apps/shared_libs/book/inc
          ${RA8_ROOT}/apps/shared_libs/third_party/miniz
)

target_compile_options(
  cache_bench
  PRIVATE -O2
          -g
          -Wall
          -Wextra
          -Werror
)

target_compile_definitions(cache_bench PRIVATE MINIZ_NO_MALLOC MINIZ_NO_STDIO)

# miniz is SOUP and type-puns through byte buffers. Both pinned compiler arms
# compile these two TUs under -Wall -Wextra -Werror with zero diagnostics; the
# previous warning suppressions and blanket demotion were dead. Keep only the
# optimizer contract that prevents strict-aliasing miscompilation.
set_source_files_properties(
  ${RA8_ROOT}/apps/shared_libs/third_party/miniz/miniz.c PROPERTIES COMPILE_OPTIONS
                                                                    "-fno-strict-aliasing"
)

include(CTest)
if(BUILD_TESTING)
  # src/cache_bench_report.c is the command-line front end (it defines main); the
  # contract test drives the replay engine through inc/cache_bench.h instead.
  add_executable(
    test_cache_bench
    tests/src/test_cache_bench.c
    src/cache_bench.c
    src/cache_bench_host.c
    src/cache_bench_io.c
    src/policies.c
    src/policy_scanresist.c
    src/sweep_block.c
    src/sweep_block_backends.c
    src/sweep_block_payload.c
    src/sweep_block_report.c
    src/trace.c
    ${RA8_ROOT}/libs/ra8_mem/src/ra8_vmem.c
    ${RA8_ROOT}/libs/ra8_mem/src/ra8_keycache.c
    ${RA8_ROOT}/libs/ra8_mem/src/ra8_vsource.c
    ${RA8_ROOT}/apps/shared_libs/book/src/book.c
    ${RA8_ROOT}/apps/shared_libs/book/src/book_chunked.c
    ${RA8_ROOT}/apps/shared_libs/third_party/miniz/miniz.c
  )
  target_include_directories(
    test_cache_bench
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc
            ${RA8_ROOT}/libs/ra8_core/inc
            ${RA8_ROOT}/libs/ra8_mem/inc
            ${RA8_ROOT}/apps/shared_libs/book/inc
            ${RA8_ROOT}/apps/shared_libs/third_party/miniz
  )
  target_compile_definitions(test_cache_bench PRIVATE MINIZ_NO_MALLOC MINIZ_NO_STDIO)
  target_compile_options(
    test_cache_bench
    PRIVATE -O1
            -g
            -Wall
            -Wextra
            -Werror
  )
  add_test(NAME cache_bench_contract COMMAND test_cache_bench)
endif()
