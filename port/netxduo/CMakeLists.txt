# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
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

# The wireless twin: `netxduo_port_c6` exposes `nx_ether_driver_c6.c`, the
# NetX Duo network driver that bridges onto the ESP32-C6 over the
# `ra8_c6link` facade (`ra8_c6link_eth_send` for transmit, the facade's 802.3
# receive callback for receive) instead of the on-chip `ra8_eth` MAC. A c6
# Wi-Fi app links this rather than `netxduo_port_ra8_eth`; it shares the same
# NetX core, so `netxduo` still supplies the stack itself.
add_library(netxduo_port_c6 INTERFACE)

target_include_directories(netxduo_port_c6 INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/inc)

set_property(
  GLOBAL PROPERTY RA8_NETXDUO_C6_PORT_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/nx_ether_driver_c6.c
)
