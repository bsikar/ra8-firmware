#
# port/netxduo/CMakeLists.txt
#
# Defines the `netxduo_port_ra8_eth` interface library that exposes
# the bridge between vendored NetX Duo and the RA8 ``ra8_eth`` HAL:
#
#   - ``nx_ether_driver_ra8_eth.c`` -- NetX Duo network driver callback
#     that hands NetX-supplied MACs / packets to ``ra8_eth_open`` /
#     ``ra8_eth_write`` / ``ra8_eth_read``.
#
# There is deliberately no NetX Crypto ALT shim here. The RSIP-E50D
# symmetric-cipher and hash engines are not backed by a documented
# register interface on this silicon (HUM Ch 52 is a feature overview
# with no command-register map), so the RSIP HAL fail-closes in
# production (issues #214 / #215). NetX Crypto therefore uses its own
# built-in software AES / SHA-256 directly; the shipping crypto path is
# tf-psa-crypto on the M85.
#
# Pulled in by ``cmake/netxduo.cmake`` when ``RA8_USE_NETXDUO=ON``.
# The example app links against ``netxduo_port_ra8_eth``; it brings
# its own globbed ``ra8_hal`` sources, so this shim only needs to
# surface the include directory + the single source file we expect
# the app to compile.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

cmake_minimum_required(VERSION 3.20)

# Header-only interface for source consumers that prefer to compile
# the .c themselves (mirroring the rest of the per-app glob style).
add_library(netxduo_port_ra8_eth INTERFACE)

target_include_directories(netxduo_port_ra8_eth INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/inc)

# Track the source list as a property so apps can splice it into
# their own add_executable() globs.
set_property(
  GLOBAL PROPERTY RA8_NETXDUO_PORT_SOURCES
                  ${CMAKE_CURRENT_SOURCE_DIR}/src/nx_ether_driver_ra8_eth.c
)
