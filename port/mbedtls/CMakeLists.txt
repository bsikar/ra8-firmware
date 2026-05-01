#
# port/mbedtls/CMakeLists.txt
#
# Defines the `mbedtls_port_ra_rsip` interface library that exposes
# the bridge between vendored Mbed TLS and the RA8D2 ``ra_rsip`` HAL.
# The shim has three pieces:
#
#   - ``mbedtls_aes_alt.{h,c}``   -- ALT replacement for Mbed TLS's
#     ``mbedtls_aes_*`` public surface. Routes init / setkey / crypt
#     through ``ra_rsip_aes*_install_plain`` + ``ra_rsip_aes_cipher``.
#   - ``mbedtls_sha256_alt.{h,c}`` -- same shape, for the streaming
#     SHA-256 family ``ra_rsip_sha256_init`` / ``..._update`` /
#     ``..._final``.
#   - ``mbedtls_config.h``        -- the project-wide Mbed TLS
#     compile-time configuration (TLS 1.2 + 1.3, AES, SHA-256, RSA,
#     ECDHE, MBEDTLS_AES_ALT, MBEDTLS_SHA256_ALT, etc.).
#
# Pulled in by ``cmake/mbedtls.cmake`` when ``RA_USE_MBEDTLS=ON``.
# The example app links against ``mbedtls_port_ra_rsip``; it brings
# its own globbed ``ra_hal`` sources, so this shim only needs to
# surface the include directory + the source list as a property the
# app can splice into its own ``add_executable`` glob.
#
# The include directory is exposed as INTERFACE so consumers pick it
# up via ``target_link_libraries``. The order matters: this directory
# must come BEFORE the upstream ``libs/third_party/mbedtls/include``
# in every consumer's include search list so our ``mbedtls_aes_alt.h``
# / ``mbedtls_sha256_alt.h`` win the AES_ALT / SHA256_ALT lookups.
# ``cmake/mbedtls.cmake`` arranges that order through the
# ``mbedtls_port_ra_rsip`` target's INTERFACE_INCLUDE_DIRECTORIES
# being prepended ahead of ``mbedtls_objs``'s include set.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

cmake_minimum_required(VERSION 3.20)

# Header-only interface for source consumers that prefer to compile
# the .c themselves (mirroring the rest of the per-app glob style).
add_library(mbedtls_port_ra_rsip INTERFACE)

target_include_directories(mbedtls_port_ra_rsip INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Track the source list as a property so apps can splice it into
# their own add_executable() globs.
set_property(GLOBAL PROPERTY RA_MBEDTLS_PORT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/mbedtls_aes_alt.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mbedtls_sha256_alt.c
)

# Track the config header path so cmake/mbedtls.cmake can pin
# MBEDTLS_CONFIG_FILE to it.
set_property(GLOBAL PROPERTY RA_MBEDTLS_CONFIG_FILE
    ${CMAKE_CURRENT_SOURCE_DIR}/mbedtls_config.h
)
