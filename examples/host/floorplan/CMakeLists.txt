#
# Host floor-plan demo for the macOS display PAL backend.
#
# This is an EXAMPLE / development tool, not part of the cross-compiled
# firmware. It builds natively on macOS and links AppKit through the
# Objective-C window shim. Everything else (ra_gfx, the PAL dispatcher,
# the portable backend, and the simulator-safe ra_core paths) is the
# same source the target build uses.
#
# Build + run:
#     cmake -B build -S .
#     cmake --build build
#     ./build/floorplan
# Or simply: `make run`.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

cmake_minimum_required(VERSION 3.20)
project(ra8d2_floorplan LANGUAGES C OBJC)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(NOT APPLE)
    message(FATAL_ERROR "The floorplan demo uses the macOS window backend; build it on macOS.")
endif()

# Firmware root (three levels up from examples/host/floorplan/).
get_filename_component(FW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

# Host build: take the simulator-safe code paths in ra_core (the same
# define the unit-test build uses) so no MCU MMIO is touched.
add_compile_definitions(RA_SIMULATOR_MODE)

# Minimal ra_core: ra_log.c provides ra_log_* + ra_err_to_str; the fatal
# sink backs the RA_ASSERT / RA_ERROR_CHECK macros. We deliberately do
# NOT pull ra_time.c (its SysTick ISR references ThreadX/USBX glue the
# desktop demo neither has nor runs).
set(RA_CORE_SOURCES
    ${FW_ROOT}/libs/ra_core/src/ra_log.c
    ${FW_ROOT}/libs/ra_core/src/ra_error_handler.c
)

add_executable(floorplan
    main.c
    floorplan_png.m
    ${RA_CORE_SOURCES}
    ${FW_ROOT}/libs/ra_gfx/src/ra_gfx_text.c
    ${FW_ROOT}/libs/ra_gfx/src/ra_gfx_font_8x16.c
    ${FW_ROOT}/libs/ra_display_pal/src/ra_display_pal.c
    ${FW_ROOT}/libs/ra_display_pal/src/ra_display_pal_host_macos.c
    ${FW_ROOT}/libs/ra_display_pal/src/ra_display_pal_host_macos_pixels.c
    # Shared framebuffer->CGImage factory + the real AppKit shim
    # (NOT the headless _sim.c, which is for the unit-test build).
    ${FW_ROOT}/libs/ra_display_pal/src/ra_display_pal_host_macos_cgimage.m
    ${FW_ROOT}/libs/ra_display_pal/src/ra_display_pal_host_macos_window.m
)

target_include_directories(floorplan PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${FW_ROOT}/libs/ra_core/inc
    ${FW_ROOT}/libs/ra_gfx/inc
    ${FW_ROOT}/libs/ra_display_pal/inc
    ${FW_ROOT}/libs/ra_display_pal/src
)

# Keep our own sources strict; ra_core host paths keep their unused
# MMIO-accessor statics (no-ops under RA_SIMULATOR_MODE), so silence that.
target_compile_options(floorplan PRIVATE -Wall -Wextra -Wno-unused-function)

# The Objective-C shim needs ARC and a C23 dialect (for `bool`/`nullptr`
# used by the shared firmware headers).
set_source_files_properties(
    ${FW_ROOT}/libs/ra_display_pal/src/ra_display_pal_host_macos_cgimage.m
    ${FW_ROOT}/libs/ra_display_pal/src/ra_display_pal_host_macos_window.m
    ${CMAKE_CURRENT_SOURCE_DIR}/floorplan_png.m
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-std=gnu2x"
)

target_link_libraries(floorplan PRIVATE "-framework Cocoa")
