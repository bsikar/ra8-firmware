# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# mkbookimg -- build a FAT32 SD-card image carrying compiled .rabook files,
# written through the real ra8_fs so the firmware (ereader_shelf / ra8_emulator
# --sd) reads them back bit-for-bit.
#
#   cmake -B build -S . && cmake --build build
#   ./build/mkbookimg books.img a.rabook b.rabook ...
#
# Standalone host dev tool under tools/, outside the firmware cross-build.
#
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

# The tool is split across two TUs (file-size gate): the host container plus
# entry point, and the population pass that streams and verifies each book.
add_executable(
  mkbookimg ${CMAKE_CURRENT_SOURCE_DIR}/src/mkbookimg.c
            ${CMAKE_CURRENT_SOURCE_DIR}/src/mkbookimg_books.c ${_ra8_fs_srcs}
)

target_include_directories(
  mkbookimg PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc ${_repo}/libs/ra8_fs/inc
                    ${_repo}/libs/ra8_core/inc
)

# First-party sources carry the project warning bar (NASA Power of 10
# Rule 10) here exactly as they do in the ARM and host-test builds; only
# vendored SOUP is compiled quietly.
target_compile_options(mkbookimg PRIVATE -Wall -Wextra -Werror)

include(CTest)
if(BUILD_TESTING)
  add_test(NAME mkbookimg_integration COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.sh
                                              $<TARGET_FILE:mkbookimg> ${CMAKE_COMMAND}
  )
  set_tests_properties(mkbookimg_integration PROPERTIES TIMEOUT 45)
endif()
