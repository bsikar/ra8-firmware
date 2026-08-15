# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# exfat_mkimage -- build an exFAT volume image through the real ra8_fs, so a
# host operating system can be asked to mount it and DISPLAY the names. That
# is the one question our own byte assertions cannot answer, and it is why
# scripts/dev/exfat_macos_interop.sh drives this tool rather than borrowing
# images from the unit-test binaries on another machine.
#
#   cmake -B build -S . && cmake --build build
#   ./build/exfat_mkimage /tmp/showcase.img
#
# Standalone host dev tool under tools/, outside the firmware cross-build.

cmake_minimum_required(VERSION 3.20)
project(ra8d2_exfat_mkimage LANGUAGES C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Repo root from this tool dir (tools/exfat_mkimage -> repo).
get_filename_component(_repo "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# Glob the whole FAT/exFAT driver -- and ONLY the driver (no ra8_core_hal, so
# no MMIO and no ra8_time weak-extern), which is what lets this build and RUN
# on macOS where the unit-test harness cannot. Mirrors tools/mkfontimg and the
# same glob in tests/cmake/tests_storage.cmake.
file(GLOB _ra8_fs_fat_tu_sources CONFIGURE_DEPENDS ${_repo}/libs/ra8_fs/src/ra8_fs_fat*.c)
# The UTF codec is named for the encoding, not the ra8_fs_fat* family, so the
# glob above misses it; the ra8_fs_fat*.c TUs call into it. Name it explicitly.
list(APPEND _ra8_fs_fat_tu_sources ${_repo}/libs/ra8_fs/src/ra8_fs_utf.c)

add_executable(exfat_mkimage ${CMAKE_CURRENT_SOURCE_DIR}/exfat_mkimage.c ${_ra8_fs_fat_tu_sources})

target_include_directories(
  exfat_mkimage PRIVATE ${_repo}/libs/ra8_fs/inc ${_repo}/libs/ra8_core/inc
)

# First-party sources carry the project warning bar (NASA Power of 10 Rule 10)
# here exactly as they do in the ARM and host-test builds.
target_compile_options(exfat_mkimage PRIVATE -Wall -Wextra -Werror)

include(CTest)
if(BUILD_TESTING)
  add_test(NAME exfat_mkimage_integration
           COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.sh
                   $<TARGET_FILE:exfat_mkimage> ${CMAKE_COMMAND}
  )
  set_tests_properties(exfat_mkimage_integration PROPERTIES TIMEOUT 45)
endif()
