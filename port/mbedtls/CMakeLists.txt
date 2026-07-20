#
# port/mbedtls/CMakeLists.txt
#
# Defines the `mbedtls_port_ra8_config` interface library that exposes
# the project-supplied Mbed TLS configuration headers.
#
# This port is config-only: it ships no compiled sources. Mbed TLS 4.x
# replaced the legacy ``MBEDTLS_AES_ALT`` / ``MBEDTLS_SHA256_ALT``
# hooks with the PSA driver wrapper interface (see
# ``libs/third_party/tf-psa-crypto/scripts/data_files/driver_templates``),
# so ALT-style shims are not dispatchable, and the RSIP crypto engine
# they once targeted is not functional on this silicon (issues #214 /
# #215). The ``RA8_MBEDTLS_PORT_SOURCES`` global property is therefore
# empty. Apps that link ``mbedtls_port_ra8_config`` pick up the
# project-wide ``mbedtls_config.h`` / ``tf_psa_crypto_config.h`` via
# the include path below.
#
# Pulled in by ``cmake/mbedtls.cmake`` when ``RA8_USE_MBEDTLS=ON``.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

cmake_minimum_required(VERSION 3.20)

# Header-only interface: consumers get the config-header include path
# and nothing else.
add_library(mbedtls_port_ra8_config INTERFACE)

target_include_directories(mbedtls_port_ra8_config INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/inc)

# Config-only port: no bridge sources to splice into app builds.
# Set the property explicitly so ra8_add_app()'s generic
# RA8_<middleware>_PORT_SOURCES lookup finds a deliberate empty list.
set_property(GLOBAL PROPERTY RA8_MBEDTLS_PORT_SOURCES "")

# Track the config header path so cmake/mbedtls.cmake can pin
# MBEDTLS_CONFIG_FILE to it.
set_property(
  GLOBAL PROPERTY RA8_MBEDTLS_CONFIG_FILE ${CMAKE_CURRENT_SOURCE_DIR}/inc/mbedtls_config.h
)
