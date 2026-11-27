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

# ra8_rot is the Zig root-of-trust archive, linked via LIBS like
# ra8_psa_crypto; linking it IS the opt-in that RA8_ENABLE_ROOT_OF_TRUST used
# to be, and the define below still gates the C callers. ra8_rsip (software
# SHA-256) is auto-globbed from ra8_hal.
ra8_add_app(
  NAME rot_verify_hil
  STACK_BYTES 32768
  DESCRIPTION "On-silicon root-of-trust image verify (ECDSA-P256 via tf-psa-crypto)"
  LIBS ra8_board_ek_ra8d2 ra8_psa_crypto ra8_rot
)

# --- tf-psa-crypto software crypto, compiled standalone for ARM (SOUP) ---
# Shared recipe in cmake/tf_psa_crypto.cmake: SYSTEM includes keep the vendored
# psa headers out of -Werror + clang-tidy; the port config is software-only
# with EXTERNAL_RNG; a static-buffer allocator backs mbedtls_calloc (no libc
# heap).
include(${_d}/cmake/tf_psa_crypto.cmake)
ra8_add_tf_psa_crypto(tfpsa_rot)

target_link_libraries(rot_verify_hil.elf PRIVATE tfpsa_rot)

# Turn the root of trust on for this app (undefined in every production build).
target_compile_definitions(rot_verify_hil.elf PRIVATE RA8_ENABLE_ROOT_OF_TRUST)

# ra8_psa_crypto + ra8_rot public headers.
target_include_directories(
  rot_verify_hil.elf PRIVATE ${_d}/libs/ra8_psa_crypto/inc ${_d}/libs/ra8_dfu/inc
)

# The tf-psa include directories are SYSTEM, so its header diagnostics do not
# weaken the full project warning profile on the first-party app and RNG shim.
