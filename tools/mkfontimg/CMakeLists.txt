#
# mkfontimg -- build a FAT16 SD-card image carrying a font file, written
# through the real ra_fs so the firmware (sd_font_render / board_sim --sd)
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

# ra_fs_fat.c was split into ra_fs_fat*.c TUs for the 1000-line file-size cap
# (ra_fs_mount / ra_fs_open / ra_fs_write now live in sibling TUs), so glob them
# all to link the whole FAT driver -- and only the FAT driver (no ra_core_hal ->
# no ra_time weak-extern), so this standalone tool builds on macOS and Linux
# alike. Matches the same glob in tests/CMakeLists.txt (test_ra_fs_exfat).
file(GLOB _ra_fs_fat_tu_sources CONFIGURE_DEPENDS
    ${_repo}/libs/ra_fs/src/ra_fs_fat*.c)

add_executable(mkfontimg
    ${CMAKE_CURRENT_SOURCE_DIR}/mkfontimg.c
    ${_ra_fs_fat_tu_sources})

target_include_directories(mkfontimg PRIVATE
    ${_repo}/libs/ra_fs/inc
    ${_repo}/libs/ra_core/inc)
