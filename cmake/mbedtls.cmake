#
# cmake/mbedtls.cmake
#
# Top-level integration for the vendored Mbed TLS 4.x + TF-PSA-Crypto
# 1.x library on RA8 (Cortex-M85). Mbed TLS 4.x split the
# cryptographic primitives out of the main repo into a companion
# `tf-psa-crypto` repo: AES, SHA, RSA, ECC, bignum, PSA crypto layer
# all live there now. The mbedtls/library/ tree retains TLS, X.509,
# SSL, debug, and the MPS / net / timing helpers. Both trees are
# vendored: ``libs/third_party/mbedtls`` and
# ``libs/third_party/tf-psa-crypto``.
#
# Exposes the ``RA8_USE_MBEDTLS`` option; when ON, this file:
#
#   1. Verifies that ThreadX + NetX Duo are also enabled (Mbed TLS
#      reaches sockets through NetX Duo's ``nx_*`` API in this firmware
#      and ThreadX is required for thread-safe locking).
#   2. Compiles the union of TF-PSA-Crypto's ``core/`` +
#      ``drivers/builtin/src/`` + ``utilities/`` + ``extras/`` +
#      ``platform/`` and Mbed TLS's ``library/`` into a single
#      ``mbedtls_objs`` object library, with the project-wide
#      mbedtls + crypto config files pinned via
#      ``-DMBEDTLS_CONFIG_FILE`` / ``-DTF_PSA_CRYPTO_CONFIG_FILE`` so
#      every TU sees the same surface.
#   3. Pulls in our config-only ``port/mbedtls/`` port. The port's
#      include directory is INTERFACE-prepended on the consumer-facing
#      ``mbedtls`` target. (In Mbed TLS 4.x the legacy
#      ``MBEDTLS_AES_ALT`` / ``MBEDTLS_SHA256_ALT`` hooks have been
#      replaced by the PSA driver wrapper interface, so the port ships
#      configuration headers only -- no shim sources.)
#
# Apps that want Mbed TLS link against ``mbedtls`` and
# ``mbedtls_port_ra8_config``; everything else (include dirs, defines,
# wrap link options) flows through the interface targets.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

# Idempotency guard.
if(DEFINED _RA8_MBEDTLS_INCLUDED)
  return()
endif()
set(_RA8_MBEDTLS_INCLUDED TRUE)

option(RA8_USE_MBEDTLS "Enable the vendored Mbed TLS X.509 + TLS stack" OFF)

if(NOT RA8_USE_MBEDTLS)
  return()
endif()

if(NOT RA8_USE_THREADX)
  message(
    FATAL_ERROR
      "RA8_USE_MBEDTLS=ON requires RA8_USE_THREADX=ON. Mbed TLS uses "
      "ThreadX mutexes for thread-safe locking when bound to a "
      "multi-threaded transport (NetX Duo)."
  )
endif()

if(NOT RA8_USE_NETXDUO)
  message(
    FATAL_ERROR
      "RA8_USE_MBEDTLS=ON requires RA8_USE_NETXDUO=ON. The TLS layer "
      "is bound to NetX Duo TCP sockets via the example app's "
      "BIO callbacks; without NetX Duo there is no transport."
  )
endif()

# Resolve the repo root so this file works whether it is included
# from the top-level CMakeLists.txt or from a standalone per-app build.
get_filename_component(_RA8_MBEDTLS_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(_RA8_MBEDTLS_VENDOR_DIR "${_RA8_MBEDTLS_REPO_ROOT}/libs/third_party/mbedtls")
set(_RA8_MBEDTLS_INC_DIR "${_RA8_MBEDTLS_VENDOR_DIR}/include")
set(_RA8_MBEDTLS_LIB_DIR "${_RA8_MBEDTLS_VENDOR_DIR}/library")
set(_RA8_MBEDTLS_PORT_DIR "${_RA8_MBEDTLS_REPO_ROOT}/port/mbedtls")
set(_RA8_MBEDTLS_CONFIG_FILE "${_RA8_MBEDTLS_PORT_DIR}/inc/mbedtls_config.h")

set(_RA8_TFPSA_DIR "${_RA8_MBEDTLS_REPO_ROOT}/libs/third_party/tf-psa-crypto")
set(_RA8_TFPSA_INC_DIR "${_RA8_TFPSA_DIR}/include")
set(_RA8_TFPSA_CORE_DIR "${_RA8_TFPSA_DIR}/core")
set(_RA8_TFPSA_DISPATCH_DIR "${_RA8_TFPSA_DIR}/dispatch")
set(_RA8_TFPSA_DRIVERS_DIR "${_RA8_TFPSA_DIR}/drivers/builtin")
set(_RA8_TFPSA_DRIVERS_INC_DIR "${_RA8_TFPSA_DRIVERS_DIR}/include")
set(_RA8_TFPSA_DRIVERS_SRC_DIR "${_RA8_TFPSA_DRIVERS_DIR}/src")
set(_RA8_TFPSA_PLATFORM_DIR "${_RA8_TFPSA_DIR}/platform")
set(_RA8_TFPSA_UTILITIES_DIR "${_RA8_TFPSA_DIR}/utilities")
set(_RA8_TFPSA_EXTRAS_DIR "${_RA8_TFPSA_DIR}/extras")
set(_RA8_TFPSA_CRYPTO_CONFIG "${_RA8_MBEDTLS_PORT_DIR}/inc/tf_psa_crypto_config.h")

if(NOT EXISTS "${_RA8_MBEDTLS_INC_DIR}/mbedtls/build_info.h")
  message(FATAL_ERROR "RA8_USE_MBEDTLS=ON but Mbed TLS vendor tree is missing at "
                      "${_RA8_MBEDTLS_VENDOR_DIR}."
  )
endif()

if(NOT EXISTS "${_RA8_MBEDTLS_CONFIG_FILE}")
  message(FATAL_ERROR "RA8_USE_MBEDTLS=ON but the project Mbed TLS config "
                      "${_RA8_MBEDTLS_CONFIG_FILE} is missing."
  )
endif()

# The TF-PSA-Crypto companion vendor tree must be present. Mbed TLS 4.x
# pulls cryptographic primitives from this tree via
# `tf_psa_crypto_platform_requirements.h` and the PSA driver wrappers.
if(NOT EXISTS "${_RA8_TFPSA_CORE_DIR}/tf_psa_crypto_platform_requirements.h")
  message(
    FATAL_ERROR
      "RA8_USE_MBEDTLS=ON but the tf-psa-crypto companion vendor tree "
      "is missing at ${_RA8_TFPSA_DIR}. Vendor it from "
      "https://github.com/Mbed-TLS/TF-PSA-Crypto (matching tag for "
      "Mbed TLS 4.1.0 is TF-PSA-Crypto 1.1.0)."
  )
endif()

if(NOT EXISTS "${_RA8_TFPSA_CORE_DIR}/psa_crypto_driver_wrappers.h")
  message(
    FATAL_ERROR
      "RA8_USE_MBEDTLS=ON but the generated PSA driver wrapper header "
      "${_RA8_TFPSA_CORE_DIR}/psa_crypto_driver_wrappers.h is missing. "
      "Regenerate it via "
      "`python scripts/generate_driver_wrappers.py <out>` from the " # PATHREF-OK: upstream tree
      "tf-psa-crypto source tree (requires the `framework` git "
      "submodule + jsonschema + jinja2)."
  )
endif()

# Pull in the config-only port FIRST so its include dir is visible
# when the upstream library sources (compiled below) resolve the
# project configuration headers.
add_subdirectory(${_RA8_MBEDTLS_PORT_DIR} ${CMAKE_BINARY_DIR}/port_mbedtls)

# ---------------------------------------------------------------------------
# Source list
# ---------------------------------------------------------------------------
# Mbed TLS 4.x splits its sources across five trees. They all compile
# into a single object library so apps splice the .o set into their
# final ELF without forcing the rest of the build into the vendored
# include path.
file(GLOB _RA8_MBEDTLS_LIB_SOURCES CONFIGURE_DEPENDS "${_RA8_MBEDTLS_LIB_DIR}/*.c")

file(GLOB _RA8_TFPSA_CORE_SOURCES CONFIGURE_DEPENDS "${_RA8_TFPSA_CORE_DIR}/*.c")

file(GLOB _RA8_TFPSA_DRIVER_SOURCES CONFIGURE_DEPENDS "${_RA8_TFPSA_DRIVERS_SRC_DIR}/*.c")

file(GLOB _RA8_TFPSA_PLATFORM_SOURCES CONFIGURE_DEPENDS "${_RA8_TFPSA_PLATFORM_DIR}/*.c")

file(GLOB _RA8_TFPSA_UTIL_SOURCES CONFIGURE_DEPENDS "${_RA8_TFPSA_UTILITIES_DIR}/*.c")

file(GLOB _RA8_TFPSA_EXTRAS_SOURCES CONFIGURE_DEPENDS "${_RA8_TFPSA_EXTRAS_DIR}/*.c")

# ---------------------------------------------------------------------------
# Mbed TLS / TF-PSA-Crypto -- compiled as one OBJECT library so apps
# can splice the objects directly into their final ELF without
# dragging the vendored headers into the rest of the build's include
# path.
# ---------------------------------------------------------------------------
add_library(
  mbedtls_objs OBJECT
  ${_RA8_MBEDTLS_LIB_SOURCES}
  ${_RA8_TFPSA_CORE_SOURCES}
  ${_RA8_TFPSA_DRIVER_SOURCES}
  ${_RA8_TFPSA_PLATFORM_SOURCES}
  ${_RA8_TFPSA_UTIL_SOURCES}
  ${_RA8_TFPSA_EXTRAS_SOURCES}
)

# Order matters here: the port include dir comes first so any
# overrides we drop into ``port/mbedtls/`` win the lookup; then the
# Mbed TLS public include tree, then the TF-PSA-Crypto public
# include tree, then the per-component private include trees the
# library/core/drivers sources reach into via plain quoted includes.
target_include_directories(
  mbedtls_objs
  PUBLIC ${_RA8_MBEDTLS_PORT_DIR}
         ${_RA8_MBEDTLS_INC_DIR}
         ${_RA8_TFPSA_INC_DIR}
         ${_RA8_TFPSA_DRIVERS_INC_DIR}
         ${_RA8_TFPSA_CORE_DIR}
         ${_RA8_TFPSA_DISPATCH_DIR}
         ${_RA8_TFPSA_DRIVERS_SRC_DIR}
         ${_RA8_TFPSA_UTILITIES_DIR}
         ${_RA8_TFPSA_EXTRAS_DIR}
         ${_RA8_TFPSA_PLATFORM_DIR}
         ${_RA8_MBEDTLS_LIB_DIR}
)

target_compile_definitions(mbedtls_objs PUBLIC MBEDTLS_CONFIG_FILE="${_RA8_MBEDTLS_CONFIG_FILE}")

# Pin the PSA crypto config too if the project supplies one; otherwise
# the tree falls back to its bundled default
# (`tf-psa-crypto/include/psa/crypto_config.h`).
if(EXISTS "${_RA8_TFPSA_CRYPTO_CONFIG}")
  target_compile_definitions(
    mbedtls_objs PUBLIC TF_PSA_CRYPTO_CONFIG_FILE="${_RA8_TFPSA_CRYPTO_CONFIG}"
  )
endif()

target_link_libraries(mbedtls_objs PRIVATE threadx)

# Vendor sources predate -Wpedantic / -Werror cleanliness; let them
# build at -w like the other vendored trees.
target_compile_options(mbedtls_objs PRIVATE -w)

# Public-facing INTERFACE target. Apps link this; everything else
# flows through.
add_library(mbedtls INTERFACE)
target_sources(mbedtls INTERFACE $<TARGET_OBJECTS:mbedtls_objs>)
target_include_directories(
  mbedtls
  INTERFACE ${_RA8_MBEDTLS_PORT_DIR}
            ${_RA8_MBEDTLS_INC_DIR}
            ${_RA8_TFPSA_INC_DIR}
            ${_RA8_TFPSA_DRIVERS_INC_DIR}
            ${_RA8_TFPSA_CORE_DIR}
            ${_RA8_TFPSA_DISPATCH_DIR}
)
target_compile_definitions(mbedtls INTERFACE MBEDTLS_CONFIG_FILE="${_RA8_MBEDTLS_CONFIG_FILE}")
if(EXISTS "${_RA8_TFPSA_CRYPTO_CONFIG}")
  target_compile_definitions(
    mbedtls INTERFACE TF_PSA_CRYPTO_CONFIG_FILE="${_RA8_TFPSA_CRYPTO_CONFIG}"
  )
endif()
target_link_libraries(mbedtls INTERFACE threadx)

message(STATUS "Mbed TLS enabled:    ${_RA8_MBEDTLS_VENDOR_DIR}")
message(STATUS "TF-PSA-Crypto:       ${_RA8_TFPSA_DIR}")
message(STATUS "Mbed TLS config:     ${_RA8_MBEDTLS_CONFIG_FILE}")
if(EXISTS "${_RA8_TFPSA_CRYPTO_CONFIG}")
  message(STATUS "TF-PSA-Crypto config: ${_RA8_TFPSA_CRYPTO_CONFIG}")
else()
  message(STATUS "TF-PSA-Crypto config: <upstream default>")
endif()
