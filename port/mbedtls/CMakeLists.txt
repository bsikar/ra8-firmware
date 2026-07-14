#
# port/mbedtls/CMakeLists.txt
#
# Defines the `mbedtls_port_ra8_rsip` interface library that exposes
# the project-supplied Mbed TLS configuration headers and the
# scaffolding for routing AES + SHA-256 primitives through the RA8
# ``ra8_rsip`` HAL.
#
# Mbed TLS 4.x replaced the legacy ``MBEDTLS_AES_ALT`` /
# ``MBEDTLS_SHA256_ALT`` hooks with the PSA driver wrapper interface
# (see ``libs/third_party/tf-psa-crypto/scripts/data_files/driver_templates``).
# The previous ALT-style shim sources (``mbedtls_aes_alt.{c,h}`` /
# ``mbedtls_sha256_alt.{c,h}``) are kept in this directory as
# scaffolding for a future RSIP-via-PSA-driver wave but they are
# *not* compiled into the firmware: their public surface no longer
# matches anything Mbed TLS 4.x dispatches to. The
# ``RA8_MBEDTLS_PORT_SOURCES`` global property is therefore empty in
# this version. Apps that link ``mbedtls_port_ra8_rsip`` still pick
# up the project-wide ``mbedtls_config.h`` /
# ``tf_psa_crypto_config.h`` via the include path below.
#
# Pulled in by ``cmake/mbedtls.cmake`` when ``RA8_USE_MBEDTLS=ON``.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

cmake_minimum_required(VERSION 3.20)

# Header-only interface for source consumers that prefer to compile
# the .c themselves (mirroring the rest of the per-app glob style).
add_library(mbedtls_port_ra8_rsip INTERFACE)

target_include_directories(mbedtls_port_ra8_rsip INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)

# In Mbed TLS 4.x the ALT-style RSIP shims are not dispatched to. The
# scaffolding files (mbedtls_aes_alt.{c,h}, mbedtls_sha256_alt.{c,h})
# remain in this directory for the future PSA-driver port but are not
# compiled. Clear the property so per-app globs find an empty list.
set_property(GLOBAL PROPERTY RA8_MBEDTLS_PORT_SOURCES "")

# Track the config header path so cmake/mbedtls.cmake can pin
# MBEDTLS_CONFIG_FILE to it.
set_property(GLOBAL PROPERTY RA8_MBEDTLS_CONFIG_FILE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc/mbedtls_config.h
)
