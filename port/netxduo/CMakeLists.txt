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

# The IP-interface facade: the packet pool / IP instance / ARP-UDP-ICMP / DHCP
# bring-up three C6 Wi-Fi apps each carry as a near-verbatim private copy (RA8FW-319).
# It names NetX Duo types in its header, which is exactly why it lives here and
# not in `libs/ra8_wifi`: that facade stays stack-free and host-testable, and its
# `ra8_wifi_ip_bind_fn` hook exists so an app can take the radio without the IP
# stack coming with it. A consumer opts in by linking `netxduo_ipif` and
# splicing RA8_NETXDUO_IPIF_SOURCES into its own glob, the same shape the two
# link drivers above use. The DHCP client itself is not in the netxduo core
# glob (it lives under addons/dhcp), so a consumer still names that TU.
add_library(netxduo_ipif INTERFACE)

target_include_directories(
  netxduo_ipif INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/inc
                         ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ra8_core/inc
                         ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ra8_wifi/inc
                         ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/third_party/netxduo/addons/dhcp
)

set_property(GLOBAL PROPERTY RA8_NETXDUO_IPIF_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/ra8_ipif.c)

# The Wi-Fi provider for that facade: `ra8_ipif_wifi_bind`, the
# `ra8_wifi_ip_bind_fn` the `ra8_wifi` header promises ships "alongside the
# backend when a stack is available" (RA8FW-319). It is its own target rather than a
# second source file on `netxduo_ipif` because it names `nx_ether_driver_c6` and
# `ra8_c6link_t`: a consumer running the same bring-up over the on-chip MAC
# takes `netxduo_ipif` alone and links neither. Linking this one implies
# `netxduo_port_c6` for the driver itself.
add_library(netxduo_ipif_wifi INTERFACE)

target_link_libraries(netxduo_ipif_wifi INTERFACE netxduo_ipif netxduo_port_c6)

target_include_directories(
  netxduo_ipif_wifi INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/inc
                              ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ra8_c6link/inc
)

set_property(
  GLOBAL PROPERTY RA8_NETXDUO_IPIF_WIFI_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/ra8_ipif_wifi.c
)
