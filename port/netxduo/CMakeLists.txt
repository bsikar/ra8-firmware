#
# port/netxduo/CMakeLists.txt
#
# Defines the `netxduo_port_ra_eth` interface library that exposes
# the bridge between vendored NetX Duo and the RA8D2 ``ra_eth`` /
# ``ra_rsip`` HALs. The shim has three pieces:
#
#   - ``nx_ether_driver_ra_eth.c`` -- NetX Duo network driver callback
#     that hands NetX-supplied MACs / packets to ``ra_eth_open`` /
#     ``ra_eth_write`` / ``ra_eth_read``.
#   - ``nx_crypto_aes_alt.c`` -- ALT replacement for NetX Crypto's AES
#     algorithm methods. Routes init / operation / cleanup through
#     ``ra_rsip_aes_cipher`` so the RSIP engine performs the rounds.
#   - ``nx_crypto_sha256_alt.c`` -- same shape, for ``ra_rsip_sha256``.
#
# Pulled in by ``cmake/netxduo.cmake`` when ``RA_USE_NETXDUO=ON``.
# The example app links against ``netxduo_port_ra_eth``; it brings
# its own globbed ``ra_hal`` sources, so this shim only needs to
# surface the include directory + the three source files we expect
# the app to compile.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

cmake_minimum_required(VERSION 3.20)

# Header-only interface for source consumers that prefer to compile
# the .c themselves (mirroring the rest of the per-app glob style).
add_library(netxduo_port_ra_eth INTERFACE)

target_include_directories(netxduo_port_ra_eth INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Track the source list as a property so apps can splice it into
# their own add_executable() globs.
set_property(GLOBAL PROPERTY RA_NETXDUO_PORT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/nx_ether_driver_ra_eth.c
    ${CMAKE_CURRENT_SOURCE_DIR}/nx_crypto_aes_alt.c
    ${CMAKE_CURRENT_SOURCE_DIR}/nx_crypto_sha256_alt.c
)
