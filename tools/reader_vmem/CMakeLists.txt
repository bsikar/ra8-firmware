# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

cmake_minimum_required(VERSION 3.20)
project(reader_vmem LANGUAGES C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

get_filename_component(RA8_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
file(GLOB READER_VMEM_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")

# ra8_vsource is Zig now, so the reader links the ra8_mem archive instead of
# compiling that file by path. The archive is a single object, so it brings
# along every extern in it (ra8_vmem_get/ra8_vmem_put from the ported stream
# adapter, the four ra8_keycache_* from the glyph atlas); this target already
# compiles both of those C files, so they resolve.
if(NOT TARGET ra8_zig::ra8_mem)
  include(${RA8_ROOT}/tests/cmake/zig_library.cmake)
  ra8_add_zig_library(
    NAME ra8_mem
    ZIG_ROOT ${RA8_ROOT}/libs/ra8_mem
    LIBRARY_NAME ra8_mem
  )
endif()

add_executable(
  reader_vmem
  ${READER_VMEM_SOURCES} ${RA8_ROOT}/libs/ra8_mem/src/ra8_keycache.c
)
target_link_libraries(reader_vmem PRIVATE ra8_zig::ra8_mem)
target_include_directories(
  reader_vmem PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc ${RA8_ROOT}/libs/ra8_core/inc
                      ${RA8_ROOT}/libs/ra8_mem/inc
)
target_compile_options(
  reader_vmem
  PRIVATE -O2
          -g
          -Wall
          -Wextra
          -Werror
)
