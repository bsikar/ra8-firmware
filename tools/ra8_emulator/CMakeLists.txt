#
# RA8D2 board emulator -- companion dev tool.
#
# Loads an EK-RA8D2 firmware ELF into an emulated Cortex-M memory map (Unicorn)
# and boots it, with the RA8D2 peripheral space as logged MMIO and SysTick
# ticked cooperatively so delays advance. Standalone dev tool under tools/,
# outside the firmware CI gates.
#
#   cmake -B build -S . && cmake --build build -j
#   ./build/ra8_emulator ../../examples/ek_ra8d2/.../<app>/build/<app>.elf
#
# Needs Unicorn + Capstone:
#   macOS: brew install unicorn capstone
#   Linux: libunicorn-dev + libcapstone-dev (or a source install) -- found via
#          pkg-config, so the emulator smoke gate runs on the Linux CI runner.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

cmake_minimum_required(VERSION 3.20)

# Objective-C (board_view.m) is the macOS live-window backend ONLY, and an OBJC
# compiler / AppKit may be absent on a Linux CI runner. Enable C unconditionally
# and add OBJC only on APPLE, where board_view.m is actually compiled.
project(ra8_emulator LANGUAGES C)
if(APPLE)
  enable_language(OBJC)
endif()

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Homebrew prefix (Apple Silicon default); only used on the APPLE path below.
set(BREW /opt/homebrew)

# board_view is the optional live board window: the Cocoa backend (board_view.m)
# on macOS, the headless no-op shim (board_view_stub.c) everywhere else. Both
# satisfy the same plain-C ABI in board_view.h, so exactly one is compiled and
# the non-APPLE build links zero AppKit/CoreGraphics symbols; --view then
# cleanly falls back to headless (board_view_open returns nullptr and main.c
# continues without a window). board_periph.c is the peripheral-block registry
# CORE (block dispatch + ICU/NVIC routing); each board_periph_<blk>.c is one
# register-accurate peripheral block that self-registers with the core
# (GPIO/PORT, GPT+AGT timers, SCI_B UART, I3C/I2C+GT911, ...); board_usb.c is the
# USBFS controller model + virtual host (chapter-9 enumeration); board_overlay.c
# composes the panel + status-sidebar board view into one RGB565 buffer for both
# --view and --ppm; rest is C.
#
# Adding a peripheral block: just drop a new src/board_periph_<blk>.c (it
# self-registers its descriptor with the core via [[gnu::constructor]]
# -- see board_periph_block.h). The glob below picks it up automatically; the
# core keeps no central block list, so blocks can be added in parallel with no
# edit to this file and no conflict.
file(GLOB BOARD_PERIPH_BLOCKS CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/periph/board_periph_*.c"
)

if(APPLE)
  set(BOARD_VIEW_SRC src/display/board_view.m)
else()
  set(BOARD_VIEW_SRC src/display/board_view_stub.c)
endif()

add_executable(
  ra8_emulator
  src/main.c
  src/engine/emu_console.c
  src/engine/emu_elf.c
  src/engine/emu_engine.c
  src/engine/emu_memmap.c
  src/engine/emu_mmio.c
  src/engine/emu_mpu.c
  src/engine/emu_prof.c
  src/engine/emu_args.c
  src/engine/emu_run.c
  src/engine/emu_run_guards.c
  src/engine/emu_run_inner.c
  src/engine/emu_run_report.c
  src/engine/emu_trace.c
  src/engine/emu_tz.c
  src/engine/emu_view.c
  src/engine/emu_usbh_seam.c
  src/engine/emu_cpu1.c
  src/engine/emu_exc.c
  src/engine/emu_exc_scs.c
  src/engine/emu_idle.c
  src/engine/emu_insn_seams.c
  src/engine/emu_seam_div0.c
  src/engine/emu_seam_longshift.c
  src/engine/emu_seam_sd.c
  src/engine/emu_seam_mve.c
  src/periph/board_periph.c
  ${BOARD_PERIPH_BLOCKS}
  src/usb/board_usb.c
  src/usb/board_usb_bridge.c
  src/usb/board_usb_dev.c
  src/usb/board_usb_loop.c
  src/usb/board_usb_vhost.c
  src/usb/board_usb_host.c
  src/io/board_net.c
  src/display/board_overlay.c
  src/display/board_overlay_draw.c
  src/io/board_console.c
  src/io/board_input.c
  ${BOARD_VIEW_SRC}
)

if(APPLE)
  set_source_files_properties(src/display/board_view.m PROPERTIES COMPILE_FLAGS "-fobjc-arc")
endif()

# The USB model includes the firmware's register/ELC headers for the exact
# offsets + ELC event numbers it must mirror (ra8_usb_regs.h, ra8_elc_regs.h);
# ra8_core/inc supplies ra8_attributes.h for the RA8_PRIV annotations on the
# emu_* module-private cross-TU helpers.
target_include_directories(
  ra8_emulator
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ra8_hal/inc
          ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ra8_core/inc
)

# Unicorn + Capstone discovery: Homebrew prefix on macOS (unchanged); on Linux
# use CMake's native find_* (works with or without pkg-config). Capstone is the
# distro -dev package (disassembly only). Unicorn is the PINNED source build in
# /usr/local (docs/TOOLCHAIN.md, #354): find_library(unicorn) resolves it there
# ahead of any stale distro package, and the emulator gates fail loudly via
# check_unicorn_version.sh if the runtime Unicorn is not the pin. A custom prefix
# still works via CMAKE_PREFIX_PATH. Cocoa is linked on the APPLE path only.
if(APPLE)
  target_include_directories(ra8_emulator PRIVATE ${BREW}/include)
  target_link_directories(ra8_emulator PRIVATE ${BREW}/lib)
  target_link_libraries(
    ra8_emulator PRIVATE unicorn capstone "-framework Cocoa" "-framework QuartzCore"
  )
else()
  find_path(UNICORN_INCLUDE_DIR unicorn/unicorn.h REQUIRED)
  find_path(CAPSTONE_INCLUDE_DIR capstone/capstone.h REQUIRED)
  find_library(UNICORN_LIBRARY unicorn REQUIRED)
  find_library(CAPSTONE_LIBRARY capstone REQUIRED)
  target_include_directories(ra8_emulator PRIVATE ${UNICORN_INCLUDE_DIR} ${CAPSTONE_INCLUDE_DIR})
  target_link_libraries(ra8_emulator PRIVATE ${UNICORN_LIBRARY} ${CAPSTONE_LIBRARY})
endif()

target_compile_options(ra8_emulator PRIVATE -Wall -Wextra)
