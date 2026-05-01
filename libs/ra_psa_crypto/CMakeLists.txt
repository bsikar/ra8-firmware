#
# libs/ra_psa_crypto -- application-level PSA Crypto facade over the
# vendored TF-PSA-Crypto 1.x library.
#
# Two consumption paths (mirrors libs/ra_tls/CMakeLists.txt):
#
#   1. Host unit tests (tests/CMakeLists.txt) glob libs/ra_psa_crypto/src/*.c
#      directly into ra_core_hal so the test build never has to link the
#      real TF-PSA-Crypto. ra_psa_crypto.c uses #ifdef RA_SIMULATOR_MODE
#      to substitute small in-tree stand-ins (real SHA-256 + a sim AEAD)
#      so round-trip tests pass without the heavy crypto core.
#
#   2. Per-app firmware builds (examples/<app>/CMakeLists.txt) opt in
#      to RA_USE_MBEDTLS=ON, which pulls in cmake/mbedtls.cmake. That
#      makes the `tfpsacrypto` interface target available; this file
#      then defines a STATIC `ra_psa_crypto` target linked against it.
#
# When TF-PSA-Crypto is not available at configure time the
# add_library() call is skipped so the file is safe to include from
# both build trees.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

cmake_minimum_required(VERSION 3.20)

# Only define the STATIC target when the TF-PSA-Crypto dependency is wired up.
# The host unit-test build globs the source directly and skips this branch.
if(TARGET tfpsacrypto)
    add_library(ra_psa_crypto STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ra_psa_crypto.c
    )

    target_include_directories(ra_psa_crypto PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/inc
    )

    target_link_libraries(ra_psa_crypto PUBLIC
        ra_core
        tfpsacrypto
    )
endif()
