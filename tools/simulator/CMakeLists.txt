#
# RA8D2 UI simulator -- a config-driven companion development tool (macOS).
#
# Standalone instrument (NOT a firmware example): it becomes the display named
# by a TOML panel config and runs the shared GUIX UI against it, so screens can
# be built and clicked on the Mac. GUIX renders into an RGB565 framebuffer via
# the host 565rgb driver behind the display PAL's display_bind_guix seam; the
# macOS window backend presents it.
#
# Build + run:
#     cmake -B build -S . && cmake --build build -j
#     ./build/sim --panel panels/ek_ra8d2.toml
# Or from the repo root: `make sim` (PANEL=<name> to pick a config).
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

cmake_minimum_required(VERSION 3.20)
project(ra8d2_simulator LANGUAGES C OBJC)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT APPLE)
    message(FATAL_ERROR "The simulator uses the macOS window backend; build it on macOS.")
endif()

# Repo root (two levels up from tools/simulator/).
get_filename_component(FW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_compile_definitions(
    RA_SIMULATOR_MODE
    RA_DISPLAY_PAL_HOST_GUIX
    GX_INCLUDE_USER_DEFINE_FILE
    GX_DISABLE_THREADX_BINDING
)

# GUIX core minus the ThreadX bind (our gx_generic_* shim replaces it).
file(GLOB GUIX_SRC CONFIGURE_DEPENDS ${FW_ROOT}/libs/third_party/guix/common/src/*.c)
list(FILTER GUIX_SRC EXCLUDE REGEX "gx_system_rtos_bind\\.c$")

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

add_executable(sim
    main.c
    sim_panel.c
    ${FW_ROOT}/examples/shared/bedroom_ui/bedroom_ui.c
    ${FW_ROOT}/port/guix/gx_display_driver_host.c
    ${FW_ROOT}/port/guix/host/gx_generic_rtos_host.c
    ${RA_CORE_SOURCES}
    ${PAL_SOURCES}
    ${GUIX_SRC}
)

target_include_directories(sim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${FW_ROOT}/examples/shared/bedroom_ui
    ${FW_ROOT}/libs/ra_board_ek_ra8d2/inc                # ra_panel.h (bedroom_ui)
    ${FW_ROOT}/port/guix
    ${FW_ROOT}/port/guix/host
    ${FW_ROOT}/libs/third_party/guix/common/inc
    ${FW_ROOT}/libs/ra_core/inc
    ${FW_ROOT}/libs/ra_display_pal/inc
    ${FW_ROOT}/libs/ra_display_pal/src
)

target_compile_options(sim PRIVATE -Wall -Wextra -Wno-unused-function)

# GUIX is SOUP: compile at C11 with warnings off (per-source overrides win).
set_source_files_properties(${GUIX_SRC} PROPERTIES COMPILE_OPTIONS "-std=gnu11;-w")

# Objective-C shims need ARC + a C23 dialect.
set_source_files_properties(
    ${FW_ROOT}/libs/ra_display_pal/src/ra_display_pal_host_macos_cgimage.m
    ${FW_ROOT}/libs/ra_display_pal/src/ra_display_pal_host_macos_window.m
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-std=gnu2x"
)

target_link_libraries(sim PRIVATE "-framework Cocoa")
