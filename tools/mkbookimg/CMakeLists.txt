#
# mkbookimg -- build a FAT32 SD-card image carrying compiled .rabook files,
# written through the real ra8_fs so the firmware (ereader_shelf / board_sim
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

# ra8_fs is split across ra8_fs_fat*.c TUs (file-size gate); glob them so the
# tool keeps linking as the library gains/renames TUs.
file(GLOB _ra8_fs_srcs CONFIGURE_DEPENDS ${_repo}/libs/ra8_fs/src/*.c)

add_executable(mkbookimg ${CMAKE_CURRENT_SOURCE_DIR}/mkbookimg.c ${_ra8_fs_srcs})

target_include_directories(mkbookimg PRIVATE ${_repo}/libs/ra8_fs/inc ${_repo}/libs/ra8_core/inc)

# First-party sources carry the project warning bar (NASA Power of 10
# Rule 10) here exactly as they do in the ARM and host-test builds; only
# vendored SOUP is compiled quietly.
target_compile_options(mkbookimg PRIVATE -Wall -Wextra -Werror)
