# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMake for `rot_verify_hil` -- on-silicon root-of-trust image verify.
# Runs the REAL ra8_rot verify chain (SHA-256 -> version-bind -> ECDSA-P256
# verify via tf-psa-crypto) against a signed image fixture on the EK-RA8D2,
# proving a working root of trust can authenticate images on this hardware.
# This does NOT touch the boot path -- it is a self-test, so no brick risk.
#
#
cmake_minimum_required(VERSION 3.20)

get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(rot_verify_hil LANGUAGES C ASM)
endif()

set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

# ra8_rot.c + the real (on-target) ra8_psa_crypto.c are compiled straight into the
# app via EXTRA_SRCS; the whole root of trust is behind RA8_ENABLE_ROOT_OF_TRUST,
# defined below. ra8_rsip (software SHA-256) is auto-globbed from ra8_hal.
ra8_add_app(
  NAME rot_verify_hil
  STACK_BYTES 32768
  DESCRIPTION "On-silicon root-of-trust image verify (ECDSA-P256 via tf-psa-crypto)"
  LIBS ra8_board_ek_ra8d2
  EXTRA_SRCS ${_d}/libs/ra8_psa_crypto/src/ra8_psa_crypto.c ${_d}/libs/ra8_dfu/src/ra8_rot.c
)

# --- tf-psa-crypto software crypto, compiled standalone for ARM (SOUP) ---
# Same recipe as psa_crypto_hil: SYSTEM includes keep the vendored psa headers
# out of -Werror + clang-tidy; the port config is software-only with
# EXTERNAL_RNG; a static-buffer allocator backs mbedtls_calloc (no libc heap).
set(_TF "${_d}/libs/third_party/tf-psa-crypto")
set(_PORT "${_d}/port/mbedtls/inc")
file(
  GLOB
  _TF_SRC
  CONFIGURE_DEPENDS
  ${_TF}/core/*.c
  ${_TF}/drivers/builtin/src/*.c
  ${_TF}/platform/*.c
  ${_TF}/utilities/*.c
  ${_TF}/extras/*.c
)
add_library(tfpsa_rot STATIC ${_TF_SRC})
target_include_directories(
  tfpsa_rot SYSTEM
  PUBLIC ${_TF}/include
         ${_TF}/drivers/builtin/include
         ${_TF}/core
         ${_TF}/dispatch
         ${_TF}/drivers/builtin/src
         ${_TF}/platform
         ${_TF}/utilities
         ${_TF}/extras
         ${_d}/libs/third_party/mbedtls/include
         ${_PORT}
)
target_compile_definitions(
  tfpsa_rot
  PUBLIC MBEDTLS_CONFIG_FILE="mbedtls_config.h" TF_PSA_CRYPTO_CONFIG_FILE="tf_psa_crypto_config.h"
         MBEDTLS_PLATFORM_MEMORY MBEDTLS_MEMORY_BUFFER_ALLOC_C
)
target_compile_options(tfpsa_rot PRIVATE -w -Wno-error -fno-strict-aliasing)

target_link_libraries(rot_verify_hil.elf PRIVATE tfpsa_rot)

# Turn the root of trust on for this app (undefined in every production build).
target_compile_definitions(rot_verify_hil.elf PRIVATE RA8_ENABLE_ROOT_OF_TRUST)

# ra8_psa_crypto + ra8_rot public headers.
target_include_directories(
  rot_verify_hil.elf PRIVATE ${_d}/libs/ra8_psa_crypto/inc ${_d}/libs/ra8_dfu/inc
)

# main.c + the RNG shim include the vendored psa/crypto.h (not -Werror-clean via
# a non-SYSTEM path in a couple of spots); compile the psa-touching glue TUs
# -w -Wno-error while the rest of the app keeps -Werror.
set_source_files_properties(
  ${CMAKE_CURRENT_SOURCE_DIR}/main.c ${CMAKE_CURRENT_SOURCE_DIR}/src/psa_kat_shim.c
  PROPERTIES COMPILE_OPTIONS "-w;-Wno-error"
)
