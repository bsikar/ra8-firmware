# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# port/levelx/CMakeLists.txt
#
# Defines the `levelx_port_ra8_xspi` interface library that exposes the
# bridge between vendored LevelX and our `ra8_xspi` HAL. The shim is a
# thin (~250 LOC) translation of LevelX's NOR driver-callback contract
# into matching `ra8_xspi_flash_*` polled calls against the EK-RA8D2
# ISSI IS25LX512M octal-SPI flash chip.
#
# Pulled in by `cmake/levelx.cmake` when `RA8_USE_LEVELX=ON`. The
# example app links against `levelx_port_ra8_xspi`; it brings its own
# globbed `ra8_hal` sources, so this shim only needs to surface the
# include directory and the one source file the app must compile.
#
#

cmake_minimum_required(VERSION 3.20)

# Header-only interface for source consumers that prefer to compile
# the .c themselves (mirroring the rest of the per-app glob style).
add_library(levelx_port_ra8_xspi INTERFACE)

target_include_directories(levelx_port_ra8_xspi INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/inc)

# Track the source list as a global property so per-app builds can
# splice it into their own add_executable() globs. The raw LevelX <->
# xSPI bridge is the always-on entry; the FAT-on-LevelX adapter is
# tracked separately because it pulls in FileX headers and is only
# wanted by apps that link both FileX and LevelX.
set_property(
  GLOBAL PROPERTY RA8_LEVELX_PORT_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/lx_nor_driver_ra8_xspi.c
)
set_property(
  GLOBAL PROPERTY RA8_LEVELX_FILEX_PORT_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/lx_filex_adapter.c
)
