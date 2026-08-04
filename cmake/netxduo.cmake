# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
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
#      Ethernet driver entry point (`nx_ether_driver_ra8_eth`).
#
# NetX Crypto's AES / SHA-256 methods are NOT redirected to the RSIP
# engine. The RSIP-E50D symmetric-cipher and hash blocks are not
# backed by a documented register interface on this silicon (HUM Ch 52
# is a six-page feature overview with no command-register map), so the
# RSIP HAL fail-closes in production (issues #214 / #215). NetX Crypto
# therefore uses its own built-in software AES / SHA-256 directly, and
# the shipping crypto path is tf-psa-crypto on the M85. There is no
# `--wrap` redirection and no ALT shim: the upstream crypto methods are
# linked as-is.
#
# Apps that want NetX Duo `target_link_libraries(<app>.elf PRIVATE
# netxduo netxduo_port_ra8_eth)` -- everything else (include dirs,
# preprocessor defines) flows through the interface targets.
#
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
  message(
    FATAL_ERROR
      "RA8_USE_NETXDUO=ON requires RA8_USE_THREADX=ON. NetX Duo's "
      "protection macros call tx_mutex_get / tx_mutex_put. Enable "
      "both options (or include cmake/threadx.cmake before " "cmake/netxduo.cmake)."
  )
endif()

# Resolve the repo root so this file works whether it is included
# from the top-level CMakeLists.txt or from a standalone per-app build.
get_filename_component(_RA8_NETXDUO_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(_RA8_NETXDUO_VENDOR_DIR "${_RA8_NETXDUO_REPO_ROOT}/libs/third_party/netxduo")
set(_RA8_NETXDUO_COMMON_INC "${_RA8_NETXDUO_VENDOR_DIR}/common/inc")
set(_RA8_NETXDUO_COMMON_SRC "${_RA8_NETXDUO_VENDOR_DIR}/common/src")
set(_RA8_NETXDUO_CRYPTO_INC "${_RA8_NETXDUO_VENDOR_DIR}/crypto_libraries/inc")
set(_RA8_NETXDUO_PORT_INC "${_RA8_NETXDUO_VENDOR_DIR}/ports/cortex_m85/gnu/inc")

if(NOT EXISTS "${_RA8_NETXDUO_COMMON_INC}/nx_api.h")
  message(FATAL_ERROR "RA8_USE_NETXDUO=ON but NetX Duo vendor tree is missing at "
                      "${_RA8_NETXDUO_VENDOR_DIR}."
  )
endif()

# ---------------------------------------------------------------------------
# NetX Duo core sources.
#
# The core TCP/IP stack only. `crypto_libraries/src` is deliberately NOT
# compiled and neither is `nx_secure/`: TLS in this tree is Mbed TLS behind
# `libs/ra8_tls`, and nothing first-party calls a single `nx_crypto_*` or
# `nx_secure_*` symbol (#621). Compiling all 56 crypto TUs bought nothing --
# only `--gc-sections` kept them out of every image -- while presenting a
# second, unqualified crypto implementation inside a component whose SOUP
# record has to state what it contains.
#
# The one file that made this obvious was `nx_crypto_generic_ciphersuites.c`:
# it includes `nx_secure_tls.h`, so it had to be surgically excluded for the
# other 55 to link at all. A crypto library whose TLS-aware half cannot be
# built is not being used as a crypto library.
#
# `crypto_libraries/inc` STAYS on the include path: `common/inc/nx_api.h`
# includes the crypto headers for the `NX_CRYPTO_METHOD` struct definition, so
# the core does not compile without them. Struct layout is all it needs -- the
# core references only the `nx_crypto_algorithm` / `nx_crypto_block_size_in_bytes`
# FIELDS, never a crypto function.
# ---------------------------------------------------------------------------
file(GLOB _RA8_NETXDUO_COMMON_SOURCES CONFIGURE_DEPENDS "${_RA8_NETXDUO_COMMON_SRC}/*.c")

set(_RA8_NETXDUO_SECURE_INC "${_RA8_NETXDUO_VENDOR_DIR}/nx_secure/inc")

add_library(netxduo_objs OBJECT ${_RA8_NETXDUO_COMMON_SOURCES})

target_include_directories(
  netxduo_objs PUBLIC ${_RA8_NETXDUO_COMMON_INC} ${_RA8_NETXDUO_CRYPTO_INC}
                      ${_RA8_NETXDUO_SECURE_INC} ${_RA8_NETXDUO_PORT_INC}
)

target_link_libraries(netxduo_objs PRIVATE threadx)

# Vendor sources predate -Wpedantic / -Werror cleanliness.
target_compile_options(netxduo_objs PRIVATE -w)

add_library(netxduo INTERFACE)
target_sources(netxduo INTERFACE $<TARGET_OBJECTS:netxduo_objs>)
target_include_directories(
  netxduo INTERFACE ${_RA8_NETXDUO_COMMON_INC} ${_RA8_NETXDUO_CRYPTO_INC}
                    ${_RA8_NETXDUO_SECURE_INC} ${_RA8_NETXDUO_PORT_INC}
)
target_link_libraries(netxduo INTERFACE threadx)

# Now pull in our ra8_eth <-> NetX Duo Ethernet-driver shim.
add_subdirectory(${_RA8_NETXDUO_REPO_ROOT}/port/netxduo ${CMAKE_BINARY_DIR}/port_netxduo)

message(STATUS "NetX Duo enabled: ${_RA8_NETXDUO_VENDOR_DIR}")
