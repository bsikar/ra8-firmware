#
# port/guix/CMakeLists.txt
#
# Defines the `guix_port_ra_glcdc` interface library that exposes the
# bridge between vendored GUIX and our `ra_glcdc` framebuffer + the
# `ra_gfx` software pixel pusher. The shim is a thin (~250 LOC)
# translation of GUIX's display-driver function-pointer table into
# matching `ra_gfx_*` calls plus a single `ra_glcdc_init` swap when
# the canvas wants to flip framebuffers.
#
# Pulled in by `cmake/guix.cmake` when `RA_USE_GUIX=ON`. The example
# app links against `guix_port_ra_glcdc`; that target tracks the source
# list as a global property so apps can splice it into their existing
# add_executable() globs the same way they consume FileX's shim.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

cmake_minimum_required(VERSION 3.20)

# Header-only interface for source consumers that prefer to compile the
# .c themselves (mirroring port/filex/CMakeLists.txt).
add_library(guix_port_ra_glcdc INTERFACE)

target_include_directories(guix_port_ra_glcdc INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Track the source list as a property so apps can splice it into their
# own add_executable() globs.
set_property(GLOBAL PROPERTY RA_GUIX_PORT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/gx_display_driver_ra_glcdc.c
)
