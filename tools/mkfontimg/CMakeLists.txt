#
# mkfontimg -- build a FAT16 SD-card image carrying a font file, written
# through the real ra8_fs so the firmware (sd_font_render / board_sim --sd)
# reads it back bit-for-bit.
#
#   cmake -B build -S . && cmake --build build
#   ./build/mkfontimg ../../libs/fonts/Literata-Regular.ttf font.img FONT.OTF
#
# Standalone host dev tool under tools/, outside the firmware cross-build.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
cmake_minimum_required(VERSION 3.20)
project(ra8d2_mkfontimg LANGUAGES C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Repo root from this tool dir (tools/mkfontimg -> repo).
get_filename_component(_repo "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# ra8_fs_fat.c was split into ra8_fs_fat*.c TUs for the 1000-line file-size cap
# (ra8_fs_mount / ra8_fs_open / ra8_fs_write now live in sibling TUs), so glob them
# all to link the whole FAT driver -- and only the FAT driver (no ra8_core_hal ->
# no ra8_time weak-extern), so this standalone tool builds on macOS and Linux
# alike. Matches the same glob in tests/CMakeLists.txt (test_ra8_fs_exfat).
file(GLOB _ra8_fs_fat_tu_sources CONFIGURE_DEPENDS
    ${_repo}/libs/ra8_fs/src/ra8_fs_fat*.c)

add_executable(mkfontimg
    ${CMAKE_CURRENT_SOURCE_DIR}/mkfontimg.c
    ${_ra8_fs_fat_tu_sources})

target_include_directories(mkfontimg PRIVATE
    ${_repo}/libs/ra8_fs/inc
    ${_repo}/libs/ra8_core/inc)

# First-party sources carry the project warning bar (NASA Power of 10
# Rule 10) here exactly as they do in the ARM and host-test builds; only
# vendored SOUP is compiled quietly.
target_compile_options(mkfontimg PRIVATE -Wall -Wextra -Werror)
