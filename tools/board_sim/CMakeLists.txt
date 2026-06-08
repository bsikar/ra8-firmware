#
# RA8D2 board emulator -- companion dev tool.
#
# Loads an EK-RA8D2 firmware ELF into an emulated Cortex-M memory map (Unicorn)
# and boots it, with the RA8D2 peripheral space as logged MMIO and SysTick
# ticked cooperatively so delays advance. Standalone dev tool under tools/,
# outside the firmware CI gates.
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
project(ra8d2_board_sim LANGUAGES C OBJC)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Homebrew prefix (Apple Silicon default).
set(BREW /opt/homebrew)

# board_view.m is the optional live macOS window (--view); board_periph.c is the
# register-accurate peripheral-model framework (GPIO/timers/ICU); board_usb.c is
# the USBFS controller model + virtual host (chapter-9 enumeration); rest is C.
add_executable(board_sim main.c board_periph.c board_usb.c board_view.m)
set_source_files_properties(board_view.m PROPERTIES COMPILE_FLAGS "-fobjc-arc")
# The USB model includes the firmware's register/ELC headers for the exact
# offsets + ELC event numbers it must mirror (ra8d2_usb_regs.h, ra8d2_elc_regs.h).
target_include_directories(board_sim PRIVATE ${BREW}/include
                           ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ra_hal/inc)
target_link_directories(board_sim PRIVATE ${BREW}/lib)
target_link_libraries(board_sim PRIVATE unicorn capstone "-framework Cocoa")
target_compile_options(board_sim PRIVATE -Wall -Wextra)
