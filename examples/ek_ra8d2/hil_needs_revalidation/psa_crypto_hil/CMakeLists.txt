# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMake for `psa_crypto_hil` -- on-silicon NIST/RFC known-answer test
# for the REAL tf-psa-crypto software backend (SHA-256 + AES-128-GCM +
# ECDSA-P256 verify) on the EK-RA8D2. Proves that the crypto the root of trust
# needs actually works on the M85, using the software backend (the RSIP HASH
# hardware is non-functional -- see libs/ra8_hal/src/ra8_rsip.c).
#
#
cmake_minimum_required(VERSION 3.20)

get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(psa_crypto_hil LANGUAGES C ASM)
endif()

set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

ra8_add_app(
  NAME psa_crypto_hil
  STACK_BYTES 32768
  DESCRIPTION "On-silicon NIST/RFC KAT for the tf-psa-crypto software backend"
  LIBS ra8_board_ek_ra8d2
)

# ---------------------------------------------------------------------------
# tf-psa-crypto software crypto, compiled standalone for the ARM target.
# SOUP: build it -w -fno-strict-aliasing, isolated from the app's -Werror. The
# vendored port config already enables MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG and no
# hardware-accelerator dispatch, so this is pure software crypto; the app
# supplies a test-only mbedtls_psa_external_get_random + mbedtls_ms_time
# (src/psa_kat_shim.c). MBEDTLS_PSA_CRYPTO_STORAGE_C is off -> no filesystem.
# ---------------------------------------------------------------------------
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
add_library(tfpsa_arm STATIC ${_TF_SRC})
# SYSTEM so consumers (main.c, the shim) and clang-tidy treat the vendored psa
# headers as system headers -- no -Werror, no tidy diagnostics from SOUP headers.
target_include_directories(
  tfpsa_arm SYSTEM
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
  tfpsa_arm
  PUBLIC MBEDTLS_CONFIG_FILE="mbedtls_config.h"
         TF_PSA_CRYPTO_CONFIG_FILE="tf_psa_crypto_config.h"
         # The firmware forbids libc heap (ra8_sbrk_trap, NASA Rule 3). Route
         # tf-psa's mbedtls_calloc through a static-buffer allocator the app seeds
         # (mbedtls_memory_buffer_alloc_init) so psa_crypto_init never calls sbrk.
         MBEDTLS_PLATFORM_MEMORY
         MBEDTLS_MEMORY_BUFFER_ALLOC_C
)
# No warning suppression: this OBJECT library has no warning profile to suppress.
# ra8_add_app() applies the project profile to the app target, not to this
# separately-declared tf-psa-crypto object library, so its compile line is the
# toolchain flags plus this one. Compiling all 77 tf-psa-crypto TUs with
# -Wno-attributes and -Wno-redundant-decls removed, under arm-none-eabi-gcc
# 13.3.1 at -O0, emits nothing at all: neither -Wattributes nor
# -Wredundant-decls is ever enabled here. Both are deleted; -fno-strict-aliasing
# stays because it changes code generation, not diagnostics.
target_compile_options(tfpsa_arm PRIVATE -fno-strict-aliasing)

target_link_libraries(psa_crypto_hil.elf PRIVATE tfpsa_arm)

# The tf-psa include directories are SYSTEM, so its header diagnostics do not
# weaken the full project warning profile on the first-party app and RNG shim.
