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
# tf-psa-crypto software crypto, compiled standalone for the ARM target by the
# shared recipe in cmake/tf_psa_crypto.cmake. The vendored port config already
# enables MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG and no hardware-accelerator dispatch,
# so this is pure software crypto; the app supplies a test-only
# mbedtls_psa_external_get_random + mbedtls_ms_time (src/psa_kat_shim.c).
# MBEDTLS_PSA_CRYPTO_STORAGE_C is off -> no filesystem. The recipe routes
# tf-psa's mbedtls_calloc through a static-buffer allocator the app seeds
# (mbedtls_memory_buffer_alloc_init) because the firmware forbids libc heap
# (ra8_sbrk_trap, NASA Rule 3), so psa_crypto_init never calls sbrk.
# ---------------------------------------------------------------------------
include(${_d}/cmake/tf_psa_crypto.cmake)
ra8_add_tf_psa_crypto(tfpsa_arm)

target_link_libraries(psa_crypto_hil.elf PRIVATE tfpsa_arm)

# The tf-psa include directories are SYSTEM, so its header diagnostics do not
# weaken the full project warning profile on the first-party app and RNG shim.
