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

add_executable(
  reader_vmem
  ${READER_VMEM_SOURCES} ${RA8_ROOT}/libs/ra8_mem/src/ra8_vmem.c
  ${RA8_ROOT}/libs/ra8_mem/src/ra8_keycache.c ${RA8_ROOT}/libs/ra8_mem/src/ra8_vsource.c
)
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
