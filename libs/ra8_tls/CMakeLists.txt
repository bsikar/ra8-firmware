#
# libs/ra8_tls -- thin TLS facade over the vendored Mbed TLS 4.x +
# TF-PSA-Crypto 1.x libraries.
#
# Two consumption paths:
#
#   1. Host unit tests (tests/CMakeLists.txt) glob libs/ra8_tls/src/*.c
#      directly into ra8_core_hal so the test build never has to link
#      Mbed TLS. ra8_tls.c uses #ifdef RA8_SIMULATOR_MODE to short-circuit
#      every mbedtls_* call to a loopback BIO drain in that mode.
#
#   2. Per-app firmware builds (examples/<app>/CMakeLists.txt) opt in
#      to RA8_USE_MBEDTLS=ON, which pulls in cmake/mbedtls.cmake and
#      makes the `mbedtls` + `mbedtls_port_ra8_config` interface targets
#      available. Those apps add_subdirectory() this file and link
#      against the resulting `ra8_tls` STATIC target.
#
# When Mbed TLS is not available at configure time (RA8_USE_MBEDTLS=OFF
# or it has not been included yet) the add_library() call is skipped
# so this file is safe to include from both build trees.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

cmake_minimum_required(VERSION 3.20)

# Only define the STATIC target when the Mbed TLS dependency is wired up.
# The host unit-test build globs the source directly and skips this branch.
if(TARGET mbedtls)
  add_library(ra8_tls STATIC ${CMAKE_CURRENT_SOURCE_DIR}/src/ra8_tls.c)

  target_include_directories(ra8_tls PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/inc)

  target_link_libraries(ra8_tls PUBLIC ra8_core mbedtls mbedtls_port_ra8_config)

  # tf-psa-crypto headers are exposed transitively by `mbedtls`; the
  # explicit link keeps the dependency graph honest if a future split
  # changes the propagation rules.
  if(TARGET tfpsacrypto)
    target_link_libraries(ra8_tls PUBLIC tfpsacrypto)
  endif()
endif()
