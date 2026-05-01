#
# port/levelx/CMakeLists.txt
#
# Defines the `levelx_port_ra_xspi` interface library that exposes the
# bridge between vendored LevelX and our `ra_xspi` HAL. The shim is a
# thin (~250 LOC) translation of LevelX's NOR driver-callback contract
# into matching `ra_xspi_flash_*` polled calls against the EK-RA8D2
# Macronix MX25LM512 octal-SPI flash chip.
#
# Pulled in by `cmake/levelx.cmake` when `RA_USE_LEVELX=ON`. The
# example app links against `levelx_port_ra_xspi`; it brings its own
# globbed `ra_hal` sources, so this shim only needs to surface the
# include directory and the one source file the app must compile.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

cmake_minimum_required(VERSION 3.20)

# Header-only interface for source consumers that prefer to compile
# the .c themselves (mirroring the rest of the per-app glob style).
add_library(levelx_port_ra_xspi INTERFACE)

target_include_directories(levelx_port_ra_xspi INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Track the source list as a global property so per-app builds can
# splice it into their own add_executable() globs.
set_property(GLOBAL PROPERTY RA_LEVELX_PORT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/lx_nor_driver_ra_xspi.c
)
