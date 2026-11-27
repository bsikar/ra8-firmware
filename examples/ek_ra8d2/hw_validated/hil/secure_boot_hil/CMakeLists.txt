# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMake for `secure_boot_hil` -- on-silicon enforcing secure boot.
# ra8_dfu_launch (RA8_ENABLE_ROOT_OF_TRUST) verifies ECDSA-P256 signature +
# extra-MRAM anti-rollback before copying a signed image to the SRAM run base
# and branching. Reject a tampered image, launch the genuine one. Copyright
# (c) 2026 Brighton Sikarskie. SPDX-License-Identifier: MIT
#
cmake_minimum_required(VERSION 3.20)

get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(secure_boot_hil LANGUAGES C ASM)
endif()

set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

# The whole RoT launch closure is Zig archives now: ra8_rot_launch is
# the copy-to-run gate (ra8_dfu_launch), ra8_rot the verifier and anti-rollback
# counter, ra8_psa_crypto the crypto. ra8_rsip (software SHA) is
# auto-globbed. The gate calls ra8_dfu_run_target_valid and the unauthenticated
# hand-off it wraps, both in ra8_dfu's archive, which this app asks for by hand
# below: it picks translation units out of ra8_dfu rather than naming the
# library in LIBS, so the ra8_add_app() expansion that registers a migrated
# library's archive never sees it.
ra8_add_app(
  NAME secure_boot_hil
  STACK_BYTES 32768
  DESCRIPTION "On-silicon enforcing secure boot (verify + anti-rollback via ra8_dfu_launch)"
  LIBS ra8_board_ek_ra8d2 ra8_psa_crypto ra8_rot_launch ra8_rot
       MRAM_LENGTH
       128K
       SRAM_TEXT
       ra8_flash.c
)

ra8_link_zig_library_for_cpu(
  TARGET secure_boot_hil.elf
  LIB ra8_dfu
  CPU cortex_m85
  FLOAT hard
)

# tf-psa-crypto software crypto for ARM, built by the shared recipe in
# cmake/tf_psa_crypto.cmake with this app's tf_psa_crypto_config_sb.h.
include(${_d}/cmake/tf_psa_crypto.cmake)
ra8_add_tf_psa_crypto(
  tfpsa_sb CONFIG "tf_psa_crypto_config_sb.h" CONFIG_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/inc
)

target_link_libraries(secure_boot_hil.elf PRIVATE tfpsa_sb)
target_compile_definitions(secure_boot_hil.elf PRIVATE RA8_ENABLE_ROOT_OF_TRUST)
target_include_directories(
  secure_boot_hil.elf PRIVATE ${_d}/libs/ra8_psa_crypto/inc ${_d}/libs/ra8_dfu/inc
)

# The tf-psa include directories are SYSTEM, so its header diagnostics do not
# weaken the full project warning profile on the first-party app and RNG shim.
