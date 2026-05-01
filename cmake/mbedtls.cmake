#
# cmake/mbedtls.cmake
#
# Top-level integration for the vendored Mbed TLS library on RA8D2
# (Cortex-M85). Mbed TLS owns the X.509 + TLS 1.2 / 1.3 surface; AES
# + SHA-256 are routed through our RSIP port shims (see
# ``port/mbedtls/``); RSA / ECDH stay on Mbed TLS's bignum until the
# RSIP-asym path is wired in a later sweep.
#
# Exposes the ``RA_USE_MBEDTLS`` option; when ON, this file:
#
#   1. Verifies that ThreadX + NetX Duo are also enabled (Mbed TLS
#      reaches sockets through NetX Duo's ``nx_*`` API in this firmware
#      and ThreadX is required for thread-safe locking).
#   2. Compiles ``libs/third_party/mbedtls/library/*.c`` into a
#      single ``mbedtls_objs`` object library, with the project-wide
#      config file from ``port/mbedtls/mbedtls_config.h`` pinned via
#      ``-DMBEDTLS_CONFIG_FILE`` so every TU sees the same surface.
#   3. Pulls in our ``port/mbedtls/`` shim. The port's include
#      directory is INTERFACE-prepended on the consumer-facing
#      ``mbedtls`` target so ``port/mbedtls/mbedtls_aes_alt.h`` /
#      ``port/mbedtls/mbedtls_sha256_alt.h`` shadow the upstream
#      AES_ALT / SHA256_ALT inclusion sites.
#
# Apps that want Mbed TLS link against ``mbedtls`` and
# ``mbedtls_port_ra_rsip``; everything else (include dirs, defines,
# wrap link options) flows through the interface targets.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

# Idempotency guard.
if(DEFINED _RA_MBEDTLS_INCLUDED)
    return()
endif()
set(_RA_MBEDTLS_INCLUDED TRUE)

option(RA_USE_MBEDTLS "Enable the vendored Mbed TLS X.509 + TLS stack" OFF)

if(NOT RA_USE_MBEDTLS)
    return()
endif()

if(NOT RA_USE_THREADX)
    message(FATAL_ERROR
        "RA_USE_MBEDTLS=ON requires RA_USE_THREADX=ON. Mbed TLS uses "
        "ThreadX mutexes for thread-safe locking when bound to a "
        "multi-threaded transport (NetX Duo).")
endif()

if(NOT RA_USE_NETXDUO)
    message(FATAL_ERROR
        "RA_USE_MBEDTLS=ON requires RA_USE_NETXDUO=ON. The TLS layer "
        "is bound to NetX Duo TCP sockets via the example app's "
        "BIO callbacks; without NetX Duo there is no transport.")
endif()

# Resolve the repo root so this file works whether it is included
# from the top-level CMakeLists.txt or from a standalone per-app build.
get_filename_component(_RA_MBEDTLS_REPO_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(_RA_MBEDTLS_VENDOR_DIR  "${_RA_MBEDTLS_REPO_ROOT}/libs/third_party/mbedtls")
set(_RA_MBEDTLS_INC_DIR     "${_RA_MBEDTLS_VENDOR_DIR}/include")
set(_RA_MBEDTLS_LIB_DIR     "${_RA_MBEDTLS_VENDOR_DIR}/library")
set(_RA_MBEDTLS_PORT_DIR    "${_RA_MBEDTLS_REPO_ROOT}/port/mbedtls")
set(_RA_MBEDTLS_CONFIG_FILE "${_RA_MBEDTLS_PORT_DIR}/mbedtls_config.h")

if(NOT EXISTS "${_RA_MBEDTLS_INC_DIR}/mbedtls/build_info.h")
    message(FATAL_ERROR
        "RA_USE_MBEDTLS=ON but Mbed TLS vendor tree is missing at "
        "${_RA_MBEDTLS_VENDOR_DIR}.")
endif()

if(NOT EXISTS "${_RA_MBEDTLS_CONFIG_FILE}")
    message(FATAL_ERROR
        "RA_USE_MBEDTLS=ON but the project Mbed TLS config "
        "${_RA_MBEDTLS_CONFIG_FILE} is missing.")
endif()

# The Mbed TLS 4.x vendor tree splits crypto into the companion
# `tf-psa-crypto` repo. The combined check for that companion tree
# lives in `mbedtls_common.h` which `#include`s
# `tf_psa_crypto_platform_requirements.h`. When the companion tree
# has not been vendored, defer the failure to a clear status message
# (and skip the rest of this file) instead of letting the compile
# blow up with a cryptic "file not found" later. The companion tree
# arrives as `libs/third_party/tf-psa-crypto/` in a follow-up sweep.
set(_RA_MBEDTLS_TF_PSA_HDR_PATTERNS
    "${_RA_MBEDTLS_REPO_ROOT}/libs/third_party/tf-psa-crypto/include/tf_psa_crypto_platform_requirements.h"
    "${_RA_MBEDTLS_REPO_ROOT}/libs/third_party/tf-psa-crypto/drivers/builtin/include/tf_psa_crypto_platform_requirements.h"
)
set(_RA_MBEDTLS_TF_PSA_FOUND FALSE)
foreach(_p IN LISTS _RA_MBEDTLS_TF_PSA_HDR_PATTERNS)
    if(EXISTS "${_p}")
        set(_RA_MBEDTLS_TF_PSA_FOUND TRUE)
        get_filename_component(_RA_MBEDTLS_TF_PSA_INC "${_p}" DIRECTORY)
        break()
    endif()
endforeach()
if(NOT _RA_MBEDTLS_TF_PSA_FOUND)
    message(STATUS
        "RA_USE_MBEDTLS=ON but the tf-psa-crypto companion vendor "
        "tree is missing -- the vendored mbedtls 4.x library cannot "
        "compile without it. The port shims in port/mbedtls/ are "
        "still wired in for downstream consumers, but apps that "
        "link `mbedtls` will skip themselves until the companion "
        "tree lands at libs/third_party/tf-psa-crypto/.")
    set(RA_USE_MBEDTLS OFF CACHE BOOL "Mbed TLS auto-disabled (tf-psa-crypto missing)" FORCE)
    return()
endif()

# Pull in the port shim FIRST so its include dir is visible when the
# upstream library sources (compiled below) reach for the ALT headers.
add_subdirectory(${_RA_MBEDTLS_PORT_DIR}
                 ${CMAKE_BINARY_DIR}/port_mbedtls)

# ---------------------------------------------------------------------------
# Mbed TLS library sources -- compiled as an OBJECT library so apps can
# splice the objects directly into their final ELF without dragging the
# vendored headers into the rest of the build's include path.
# ---------------------------------------------------------------------------
file(GLOB _RA_MBEDTLS_LIB_SOURCES CONFIGURE_DEPENDS
    "${_RA_MBEDTLS_LIB_DIR}/*.c")

add_library(mbedtls_objs OBJECT
    ${_RA_MBEDTLS_LIB_SOURCES}
)

# Order matters: prepend the port include dir so the upstream
# ``mbedtls/aes.h`` / ``mbedtls/sha256.h`` inclusion sites that look
# for ``mbedtls_aes_alt.h`` / ``mbedtls_sha256_alt.h`` resolve to our
# overrides.
target_include_directories(mbedtls_objs PUBLIC
    ${_RA_MBEDTLS_PORT_DIR}
    ${_RA_MBEDTLS_INC_DIR}
    ${_RA_MBEDTLS_TF_PSA_INC})

target_compile_definitions(mbedtls_objs PUBLIC
    MBEDTLS_CONFIG_FILE="${_RA_MBEDTLS_CONFIG_FILE}")

target_link_libraries(mbedtls_objs PRIVATE threadx)

# Vendor sources predate -Wpedantic / -Werror cleanliness; let them
# build at -w like the other vendored trees.
target_compile_options(mbedtls_objs PRIVATE -w)

# Public-facing INTERFACE target. Apps link this; everything else
# flows through.
add_library(mbedtls INTERFACE)
target_sources(mbedtls INTERFACE $<TARGET_OBJECTS:mbedtls_objs>)
target_include_directories(mbedtls INTERFACE
    ${_RA_MBEDTLS_PORT_DIR}
    ${_RA_MBEDTLS_INC_DIR}
    ${_RA_MBEDTLS_TF_PSA_INC})
target_compile_definitions(mbedtls INTERFACE
    MBEDTLS_CONFIG_FILE="${_RA_MBEDTLS_CONFIG_FILE}")
target_link_libraries(mbedtls INTERFACE threadx)

message(STATUS "Mbed TLS enabled: ${_RA_MBEDTLS_VENDOR_DIR}")
message(STATUS "Mbed TLS config:  ${_RA_MBEDTLS_CONFIG_FILE}")
