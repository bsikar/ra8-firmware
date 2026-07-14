#
# cmake/netxduo.cmake
#
# Top-level integration for the vendored NetX Duo TCP/IP stack and the
# bundled NetX Crypto / NetX Secure libraries on RA8 (Cortex-M85).
#
# Exposes the `RA8_USE_NETXDUO` option; when ON, this file:
#
#   1. Verifies that ThreadX is also enabled (NetX Duo's protection
#      macros call `tx_mutex_*` and threads are scheduled by ThreadX).
#   2. Compiles `libs/third_party/netxduo/common/src/*.c` plus the
#      bundled NetX Crypto sources into a single `netxduo` interface
#      library.
#   3. Pulls in our `port/netxduo/` shim which exposes the NetX Duo
#      Ethernet driver entry point (`nx_ether_driver_ra8_eth`) and the
#      `_nx_crypto_method_aes_*` / `_nx_crypto_method_sha256_*` ALT
#      replacements that route to `ra8_rsip_aes_cipher` and
#      `ra8_rsip_sha256` on real silicon.
#
# The ALT replacements use GNU ld's `--wrap=symbol` redirection rather
# than excluding the upstream TUs. Each upstream
# `_nx_crypto_method_aes_init` / `..._operation` / `..._cleanup`
# (and SHA-256 equivalent) is renamed by the linker to
# `__real__nx_crypto_method_aes_init` etc., and our shim provides
# `__wrap__nx_crypto_method_aes_init` etc. which routes the body
# through ``ra8_rsip_aes_cipher`` / ``ra8_rsip_sha256`` on AES-128 ECB
# and SHA-256, falling back to ``__real_*`` for the modes RSIP does
# not natively cover (CCM-8/12/16, GCM, XCBC). This keeps the upstream
# tree completely untouched.
#
# Apps that want NetX Duo `target_link_libraries(<app>.elf PRIVATE
# netxduo netxduo_port_ra8_eth)` -- everything else (include dirs,
# preprocessor defines, --wrap link options) flows through the
# interface targets.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

# Idempotency guard.
if(DEFINED _RA8_NETXDUO_INCLUDED)
    return()
endif()
set(_RA8_NETXDUO_INCLUDED TRUE)

option(RA8_USE_NETXDUO "Enable the vendored NetX Duo TCP/IP stack" OFF)

if(NOT RA8_USE_NETXDUO)
    return()
endif()

# NetX Duo's protection macros call `tx_mutex_get` / `tx_mutex_put`,
# so the build must also pull in ThreadX.
if(NOT RA8_USE_THREADX)
    message(FATAL_ERROR
        "RA8_USE_NETXDUO=ON requires RA8_USE_THREADX=ON. NetX Duo's "
        "protection macros call tx_mutex_get / tx_mutex_put. Enable "
        "both options (or include cmake/threadx.cmake before "
        "cmake/netxduo.cmake).")
endif()

# Resolve the repo root so this file works whether it is included
# from the top-level CMakeLists.txt or from a standalone per-app build.
get_filename_component(_RA8_NETXDUO_REPO_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(_RA8_NETXDUO_VENDOR_DIR  "${_RA8_NETXDUO_REPO_ROOT}/libs/third_party/netxduo")
set(_RA8_NETXDUO_COMMON_INC  "${_RA8_NETXDUO_VENDOR_DIR}/common/inc")
set(_RA8_NETXDUO_COMMON_SRC  "${_RA8_NETXDUO_VENDOR_DIR}/common/src")
set(_RA8_NETXDUO_CRYPTO_INC  "${_RA8_NETXDUO_VENDOR_DIR}/crypto_libraries/inc")
set(_RA8_NETXDUO_CRYPTO_SRC  "${_RA8_NETXDUO_VENDOR_DIR}/crypto_libraries/src")
set(_RA8_NETXDUO_PORT_INC    "${_RA8_NETXDUO_VENDOR_DIR}/ports/cortex_m85/gnu/inc")

if(NOT EXISTS "${_RA8_NETXDUO_COMMON_INC}/nx_api.h")
    message(FATAL_ERROR
        "RA8_USE_NETXDUO=ON but NetX Duo vendor tree is missing at "
        "${_RA8_NETXDUO_VENDOR_DIR}.")
endif()

# ---------------------------------------------------------------------------
# Common NetX Duo + NetX Crypto sources.
# ---------------------------------------------------------------------------
file(GLOB _RA8_NETXDUO_COMMON_SOURCES CONFIGURE_DEPENDS
    "${_RA8_NETXDUO_COMMON_SRC}/*.c")
file(GLOB _RA8_NETXDUO_CRYPTO_SOURCES CONFIGURE_DEPENDS
    "${_RA8_NETXDUO_CRYPTO_SRC}/*.c")

# `nx_crypto_generic_ciphersuites.c` includes `nx_secure_tls.h`,
# which we do not pull into the build. The TLS-aware ciphersuite
# tables are not needed by NetX core or the AES / SHA-256 crypto
# methods themselves -- the file just packages them for nx_secure
# consumers. Drop it so we don't have to drag the entire NetX Secure
# tree in for the TCP-echo demo.
list(FILTER _RA8_NETXDUO_CRYPTO_SOURCES
    EXCLUDE REGEX ".*/nx_crypto_generic_ciphersuites\\.c$")

set(_RA8_NETXDUO_SECURE_INC "${_RA8_NETXDUO_VENDOR_DIR}/nx_secure/inc")

add_library(netxduo_objs OBJECT
    ${_RA8_NETXDUO_COMMON_SOURCES}
    ${_RA8_NETXDUO_CRYPTO_SOURCES}
)

target_include_directories(netxduo_objs PUBLIC
    ${_RA8_NETXDUO_COMMON_INC}
    ${_RA8_NETXDUO_CRYPTO_INC}
    ${_RA8_NETXDUO_SECURE_INC}
    ${_RA8_NETXDUO_PORT_INC})

target_link_libraries(netxduo_objs PRIVATE threadx)

# Vendor sources predate -Wpedantic / -Werror cleanliness.
target_compile_options(netxduo_objs PRIVATE -w)

# The list of symbols our `port/netxduo/nx_crypto_*_alt.c` shims
# wrap. Apps inherit these flags via the `netxduo` interface target.
set(_RA8_NETXDUO_WRAP_SYMS
    _nx_crypto_method_aes_init
    _nx_crypto_method_aes_operation
    _nx_crypto_method_aes_cleanup
    _nx_crypto_method_sha256_init
    _nx_crypto_method_sha256_operation
    _nx_crypto_method_sha256_cleanup
)
set(_RA8_NETXDUO_WRAP_FLAGS "")
foreach(_sym IN LISTS _RA8_NETXDUO_WRAP_SYMS)
    list(APPEND _RA8_NETXDUO_WRAP_FLAGS "-Wl,--wrap=${_sym}")
endforeach()

add_library(netxduo INTERFACE)
target_sources(netxduo INTERFACE $<TARGET_OBJECTS:netxduo_objs>)
target_include_directories(netxduo INTERFACE
    ${_RA8_NETXDUO_COMMON_INC}
    ${_RA8_NETXDUO_CRYPTO_INC}
    ${_RA8_NETXDUO_SECURE_INC}
    ${_RA8_NETXDUO_PORT_INC})
target_link_libraries(netxduo INTERFACE threadx)
target_link_options(netxduo INTERFACE ${_RA8_NETXDUO_WRAP_FLAGS})

# Now pull in our ra8_eth + ra8_rsip <-> NetX Duo shim.
add_subdirectory(${_RA8_NETXDUO_REPO_ROOT}/port/netxduo
                 ${CMAKE_BINARY_DIR}/port_netxduo)

message(STATUS "NetX Duo enabled: ${_RA8_NETXDUO_VENDOR_DIR}")
