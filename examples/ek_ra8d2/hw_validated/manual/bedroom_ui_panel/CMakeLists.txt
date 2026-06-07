#
# Per-app CMake stub for `bedroom_ui_panel`. Names the app + its first-party
# deps via the shared cmake/ra_add_app.cmake recipe, then bolts the GUIX core
# on directly -- the SAME no-ThreadX GUIX build the host preview uses
# (examples/host/guix_tabs/CMakeLists.txt), but cross-compiled for the RA8D2.
#
# This deliberately does NOT use `USES guix`: cmake/guix.cmake hard-requires
# ThreadX (GUIX's gx_system_start binds to ThreadX). This app is single-
# threaded GUIX (GX_DISABLE_THREADX_BINDING) driven from a bare-metal loop, so
# it globs the GUIX core itself -- every common TU EXCEPT the ThreadX bind
# (gx_system_rtos_bind.c), which our gx_generic_rtos_bare.c replaces.
#
# Standalone:  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8d2.cmake
# Embedded:    pulled in by the top-level CMakeLists via add_subdirectory().
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
cmake_minimum_required(VERSION 3.20)

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
    get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra_add_app.cmake")

# ra_gfx     -> ra_gfx_pixel/ra_gfx_blit referenced by the GLCDC GUIX driver.
# ra_display_pal -> display_init / display_bind_guix (LCD/GLCDC backend).
# No `USES` middleware: GUIX is added by hand below; no ThreadX is linked.
ra_add_app(NAME bedroom_ui_panel STACK_BYTES 4400
           DESCRIPTION "Bare-metal (no ThreadX) GUIX bedroom_ui on the RA8D2 panel"
           LIBS ra_gfx ra_display_pal)

# The vendored GUIX headers trip the same strict-warning gates ra_add_app
# already relaxes for USES-middleware apps (CHAR* params, redundant decls,
# nested externs, casts that discard const). This app pulls GUIX in by hand
# (no USES) so it does not get that relaxation automatically -- apply the same
# set the recipe applies to threadx_guix_demo. The GUIX core TUs are separately
# compiled -w below; this covers our own TUs that include GUIX headers
# (bedroom_ui.c, the GLCDC driver, the bind, the LCD PAL, main.c).
target_compile_options(bedroom_ui_panel.elf PRIVATE
    -Wno-error=discarded-qualifiers
    -Wno-error=cast-qual
    -Wno-error=cast-align
    -Wno-error=redundant-decls
    -Wno-error=missing-prototypes
    -Wno-error=builtin-macro-redefined
    -Wno-error)

# ---- GUIX core (no ThreadX), mirroring examples/host/guix_tabs ---------------

# Every GUIX core TU except the ThreadX bind (gx_generic_rtos_bare.c replaces it).
file(GLOB _ra_guix_src CONFIGURE_DEPENDS
     ${RA_REPO_ROOT}/libs/third_party/guix/common/src/*.c)
list(FILTER _ra_guix_src EXCLUDE REGEX "gx_system_rtos_bind\\.c$")

target_sources(bedroom_ui_panel.elf PRIVATE
    # Shared GUIX UI -- the SAME source the macOS host preview draws.
    ${RA_REPO_ROOT}/examples/shared/bedroom_ui/bedroom_ui.c
    # Target GLCDC 565RGB GUIX display driver (analog of the host driver).
    ${RA_REPO_ROOT}/port/guix/gx_display_driver_ra_glcdc.c
    # Bare-metal single-threaded GUIX RTOS bind (target sibling of host bind).
    ${RA_REPO_ROOT}/port/guix/gx_generic_rtos_bare.c
    # GUIX core (SOUP).
    ${_ra_guix_src})

# GUIX reads our host gx_user.h (GX_DISABLE_THREADX_BINDING +
# GX_DISABLE_ERROR_CHECKING + GX_SYSTEM_TIMER_MS) and the generic single-
# threaded bind. The binding mode is tested in gx_api.h *before* it includes
# gx_user.h, so GX_DISABLE_THREADX_BINDING must also be on the command line
# (the header documents this). Same flags the host preview sets.
target_compile_definitions(bedroom_ui_panel.elf PRIVATE
    GX_INCLUDE_USER_DEFINE_FILE
    GX_DISABLE_THREADX_BINDING)

target_include_directories(bedroom_ui_panel.elf PRIVATE
    ${RA_REPO_ROOT}/examples/shared/bedroom_ui              # bedroom_ui.h
    ${RA_REPO_ROOT}/port/guix                               # gx_display_driver_ra_glcdc.h
    ${RA_REPO_ROOT}/port/guix/host                          # gx_user.h, gx_port.h, gx_host_run.h
    ${RA_REPO_ROOT}/libs/third_party/guix/common/inc)

# GUIX is SOUP: compile its core at gnu11 and silence its warnings. (Per-source
# flags come after the target flags, so -std=gnu11 / -w win for these TUs.)
set_source_files_properties(${_ra_guix_src} PROPERTIES COMPILE_OPTIONS "-std=gnu11;-w")
