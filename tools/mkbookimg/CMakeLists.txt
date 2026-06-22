#
# mkbookimg -- build a FAT32 SD-card image carrying compiled .rabook files,
# written through the real ra_fs so the firmware (ereader_shelf / board_sim
# --sd) reads them back bit-for-bit.
#
#   cmake -B build -S . && cmake --build build
#   ./build/mkbookimg books.img a.rabook b.rabook ...
#
# Standalone host dev tool under tools/, outside the firmware cross-build.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
cmake_minimum_required(VERSION 3.20)
project(ra8d2_mkbookimg LANGUAGES C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Repo root from this tool dir (tools/mkbookimg -> repo).
get_filename_component(_repo "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(mkbookimg
    ${CMAKE_CURRENT_SOURCE_DIR}/mkbookimg.c
    ${_repo}/libs/ra_fs/src/ra_fs_fat.c)

target_include_directories(mkbookimg PRIVATE
    ${_repo}/libs/ra_fs/inc
    ${_repo}/libs/ra_core/inc)
