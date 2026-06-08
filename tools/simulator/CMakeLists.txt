#
# RA8D2 UI simulator -- a config-driven companion development tool.
#
# Standalone instrument (NOT a firmware example): it becomes the display named
# by a TOML panel config and runs the shared GUIX UI against it, so screens can
# be built and clicked on the Mac. GUIX renders into an RGB565 framebuffer via
# the host 565rgb driver behind the display PAL's display_bind_guix seam.
#
# Two render paths:
#   --window  live, interactive -- presents through the host PAL's Cocoa
#             backend. macOS only (needs AppKit / CoreGraphics).
#   --png     headless -- GUIX renders into the framebuffer and main.c writes a
#             PPM. No windowing system; builds + runs on Linux for CI.
#
# On APPLE both paths build (the .m AppKit shims are compiled and -framework
# Cocoa is linked). On non-APPLE (Linux CI) the build is headless-only: the
# AppKit .m shims are dropped in favour of the no-op window shim
# (ra_display_pal_host_macos_window_sim.c), Cocoa is not linked, and --window
# is a clean error at runtime (see main.c). No Cocoa/CoreGraphics symbol is
# referenced off the APPLE path.
#
# Build + run:
#     cmake -B build -S . && cmake --build build -j
#     ./build/sim --panel panels/ek_ra8d2.toml             # live window (macOS)
#     ./build/sim --panel panels/ek_ra8d2.toml --png x.ppm # headless (any OS)
# Or from the repo root: `make sim` (PANEL=<name> to pick a config).
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

cmake_minimum_required(VERSION 3.20)

# Objective-C is only needed for the macOS AppKit window shim, and an OBJC
# compiler may be absent on a Linux CI runner. Enable C unconditionally and
# add OBJC only where the .m shims are actually compiled (APPLE).
project(ra8d2_simulator LANGUAGES C)
if(APPLE)
    enable_language(OBJC)
endif()

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

# The portable PAL pieces (no Objective-C): the dispatcher, the GUIX bridge,
# the C backend, and the RGB565->RGBA8888 pixel helper. The window shim is
# selected per-platform below.
set(PAL_SOURCES
    ${FW_ROOT}/libs/ra_display_pal/src/ra_display_pal.c
    ${FW_ROOT}/libs/ra_display_pal/src/ra_display_pal_guix.c
    ${FW_ROOT}/libs/ra_display_pal/src/ra_display_pal_host_macos.c
    ${FW_ROOT}/libs/ra_display_pal/src/ra_display_pal_host_macos_pixels.c
)

# Window shim: the real Cocoa/CoreGraphics backend on macOS, the headless
# no-op shim everywhere else. The C backend (ra_display_pal_host_macos.c)
# talks to the window only through the C-ABI in
# ra_display_pal_host_macos_window.h, so exactly one of these satisfies the
# contract and the non-APPLE build pulls in zero AppKit/CoreGraphics symbols.
if(APPLE)
    list(APPEND PAL_SOURCES
        ${FW_ROOT}/libs/ra_display_pal/src/ra_display_pal_host_macos_cgimage.m
        ${FW_ROOT}/libs/ra_display_pal/src/ra_display_pal_host_macos_window.m
    )
else()
    list(APPEND PAL_SOURCES
        ${FW_ROOT}/libs/ra_display_pal/src/ra_display_pal_host_macos_window_sim.c
    )
endif()

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

# macOS only: the Objective-C shims need ARC + a C23 dialect, and the live
# window path links AppKit. Neither is referenced on the non-APPLE build.
if(APPLE)
    set_source_files_properties(
        ${FW_ROOT}/libs/ra_display_pal/src/ra_display_pal_host_macos_cgimage.m
        ${FW_ROOT}/libs/ra_display_pal/src/ra_display_pal_host_macos_window.m
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-std=gnu2x"
    )
    target_link_libraries(sim PRIVATE "-framework Cocoa")
endif()
