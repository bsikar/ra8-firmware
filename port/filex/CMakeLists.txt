# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# port/filex/CMakeLists.txt
#
# Defines the `filex_port_ra8_sdhi` interface library that exposes the
# bridge between vendored FileX and our `ra8_sdhi` block driver. The
# shim is a thin (~150 LOC) translation of FileX's media-driver
# request switch into the matching `ra8_sdhi_*` polled call.
#
# Pulled in by `cmake/filex.cmake` when `RA8_USE_FILEX=ON`. The example
# app links against `filex_port_ra8_sdhi`; it brings its own globbed
# `ra8_hal` sources, so this shim only needs to surface the include
# directory + the one source file we expect the app to compile.
#
#

cmake_minimum_required(VERSION 3.20)

# Header-only interface for source consumers that prefer to compile
# the .c themselves (mirroring the rest of the per-app glob style).
add_library(filex_port_ra8_sdhi INTERFACE)

target_include_directories(filex_port_ra8_sdhi INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/inc)

# Track the source list as a property so apps can splice it into
# their own add_executable() globs.
set_property(
  GLOBAL PROPERTY RA8_FILEX_PORT_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/fx_media_driver_ra8_sdhi.c
)
