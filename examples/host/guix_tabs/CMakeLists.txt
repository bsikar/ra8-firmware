#
# Host GUIX 3-tab bedroom UI -- macOS preview of the EK-RA8D2 panel app.
#
# The GUIX UI (examples/shared/bedroom_ui) is the SAME source that flashes on
# the panel. Here it runs on macOS: GUIX renders into an RGB565 framebuffer
# through the host 565rgb driver behind the display PAL's display_bind_guix
# seam, and the macOS window backend (AppKit) presents it. Clicks are fed to
# GUIX as pen events; `--png <path> [--tab N]` renders one screen headless.
#
# Build + run:
#     cmake -B build -S .
#     cmake --build build -j
#     ./build/guix_tabs                       # live window
#     ./build/guix_tabs --png /tmp/tab0.png --tab 0
# Or simply: `make run`.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

cmake_minimum_required(VERSION 3.20)
project(ra8d2_guix_tabs LANGUAGES C OBJC)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(NOT APPLE)
    message(FATAL_ERROR "The guix_tabs preview uses the macOS window backend; build it on macOS.")
endif()

# Firmware root (three levels up from examples/host/guix_tabs/).
get_filename_component(FW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

# Host build:
# - RA_SIMULATOR_MODE        -> simulator-safe ra_core paths (no MCU MMIO).
# - RA_DISPLAY_PAL_HOST_GUIX -> enables the macOS backend's bind_guix slot.
# - GX_INCLUDE_USER_DEFINE_FILE / GX_DISABLE_THREADX_BINDING -> GUIX reads our
#   host gx_user.h and uses the generic single-threaded bind. The binding mode
#   is tested in gx_api.h *before* it includes gx_user.h, so it must be set on
#   the command line (the header documents this).
add_compile_definitions(
    RA_SIMULATOR_MODE
    RA_DISPLAY_PAL_HOST_GUIX
    GX_INCLUDE_USER_DEFINE_FILE
    GX_DISABLE_THREADX_BINDING
)

# Every GUIX core TU except the ThreadX bind (our gx_generic_* shim replaces it).
file(GLOB GUIX_SRC CONFIGURE_DEPENDS ${FW_ROOT}/libs/third_party/guix/common/src/*.c)
list(FILTER GUIX_SRC EXCLUDE REGEX "gx_system_rtos_bind\\.c$")

# Minimal ra_core (same set the floorplan demo links): logging + fatal sink.
set(RA_CORE_SOURCES
    ${FW_ROOT}/libs/ra_core/src/ra_log.c
    ${FW_ROOT}/libs/ra_core/src/ra_error_handler.c
)

set(PAL_SOURCES
    ${FW_ROOT}/libs/ra_display_pal/src/ra_display_pal.c
    ${FW_ROOT}/libs/ra_display_pal/src/ra_display_pal_guix.c
    ${FW_ROOT}/libs/ra_display_pal/src/ra_display_pal_host_macos.c
    ${FW_ROOT}/libs/ra_display_pal/src/ra_display_pal_host_macos_pixels.c
    ${FW_ROOT}/libs/ra_display_pal/src/ra_display_pal_host_macos_cgimage.m
    ${FW_ROOT}/libs/ra_display_pal/src/ra_display_pal_host_macos_window.m
)

add_executable(guix_tabs
    main.c
    ${FW_ROOT}/examples/shared/bedroom_ui/bedroom_ui.c
    ${FW_ROOT}/port/guix/gx_display_driver_host.c
    ${FW_ROOT}/port/guix/host/gx_generic_rtos_host.c
    ${RA_CORE_SOURCES}
    ${PAL_SOURCES}
    ${GUIX_SRC}
)

target_include_directories(guix_tabs PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${FW_ROOT}/examples/shared/bedroom_ui
    ${FW_ROOT}/port/guix                                 # gx_display_driver_host.h
    ${FW_ROOT}/port/guix/host                            # gx_user.h, gx_port.h, gx_host_run.h
    ${FW_ROOT}/libs/third_party/guix/common/inc
    ${FW_ROOT}/libs/ra_core/inc
    ${FW_ROOT}/libs/ra_display_pal/inc
    ${FW_ROOT}/libs/ra_display_pal/src
)

# Keep our own sources reasonably strict; ra_core host paths keep unused
# MMIO-accessor statics (no-ops under RA_SIMULATOR_MODE), so silence that.
target_compile_options(guix_tabs PRIVATE -Wall -Wextra -Wno-unused-function)

# GUIX is SOUP: compile its core at C11 and silence its warnings. (Per-source
# flags come after the target flags, so -std=gnu11 / -w win for these TUs.)
set_source_files_properties(${GUIX_SRC} PROPERTIES COMPILE_OPTIONS "-std=gnu11;-w")

# The Objective-C shims need ARC and a C23 dialect (bool / nullptr from the
# shared firmware headers).
set_source_files_properties(
    ${FW_ROOT}/libs/ra_display_pal/src/ra_display_pal_host_macos_cgimage.m
    ${FW_ROOT}/libs/ra_display_pal/src/ra_display_pal_host_macos_window.m
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-std=gnu2x"
)

target_link_libraries(guix_tabs PRIVATE "-framework Cocoa")
