#
# RA8D2 board emulator (stage 1) -- companion dev tool.
#
# Loads an EK-RA8D2 firmware ELF into an emulated Cortex-M memory map (Unicorn)
# and boots it, with the RA8D2 peripheral space as logged MMIO. Standalone dev
# tool under tools/, outside the firmware CI gates.
#
#   cmake -B build -S . && cmake --build build -j
#   ./build/board_sim ../../examples/ek_ra8d2/.../<app>/build/<app>.elf
#
# Needs: brew install unicorn capstone
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

cmake_minimum_required(VERSION 3.20)
project(ra8d2_board_sim LANGUAGES C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Homebrew prefix (Apple Silicon default).
set(BREW /opt/homebrew)

add_executable(board_sim main.c)
target_include_directories(board_sim PRIVATE ${BREW}/include)
target_link_directories(board_sim PRIVATE ${BREW}/lib)
target_link_libraries(board_sim PRIVATE unicorn capstone)
target_compile_options(board_sim PRIVATE -Wall -Wextra)
