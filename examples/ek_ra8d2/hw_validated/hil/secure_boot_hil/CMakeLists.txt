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

# The RoT launch closure compiled straight in with RA8_ENABLE_ROOT_OF_TRUST:
# ra8_dfu_launch + its run-target check (ra8_dfu_boot) + anti-rollback + ra8_rot +
# the real (on-target) ra8_psa_crypto. ra8_rsip (software SHA) auto-globbed.
ra8_add_app(
  NAME secure_boot_hil
  STACK_BYTES 32768
  DESCRIPTION "On-silicon enforcing secure boot (verify + anti-rollback via ra8_dfu_launch)"
  LIBS ra8_board_ek_ra8d2
  EXTRA_SRCS ${_d}/libs/ra8_psa_crypto/src/ra8_psa_crypto.c
             ${_d}/libs/ra8_dfu/src/ra8_rot.c
             ${_d}/libs/ra8_dfu/src/ra8_dfu_antirollback.c
             ${_d}/libs/ra8_dfu/src/ra8_dfu_boot.c
             ${_d}/libs/ra8_dfu/src/ra8_dfu_launch.c
)

# tf-psa-crypto software crypto for ARM (same recipe as rot_verify_hil).
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
add_library(tfpsa_sb STATIC ${_TF_SRC})
target_include_directories(
  tfpsa_sb SYSTEM
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
  tfpsa_sb
  PUBLIC MBEDTLS_CONFIG_FILE="mbedtls_config.h" TF_PSA_CRYPTO_CONFIG_FILE="tf_psa_crypto_config.h"
         MBEDTLS_PLATFORM_MEMORY MBEDTLS_MEMORY_BUFFER_ALLOC_C
)
target_compile_options(tfpsa_sb PRIVATE -w -Wno-error -fno-strict-aliasing)

target_link_libraries(secure_boot_hil.elf PRIVATE tfpsa_sb)
target_compile_definitions(secure_boot_hil.elf PRIVATE RA8_ENABLE_ROOT_OF_TRUST)
target_include_directories(
  secure_boot_hil.elf PRIVATE ${_d}/libs/ra8_psa_crypto/inc ${_d}/libs/ra8_dfu/inc
)

# main.c + the RNG shim include the vendored psa/crypto.h -- compile the
# psa-touching glue TUs -w -Wno-error; the rest of the app keeps -Werror.
set_source_files_properties(
  ${CMAKE_CURRENT_SOURCE_DIR}/main.c ${CMAKE_CURRENT_SOURCE_DIR}/src/psa_kat_shim.c
  PROPERTIES COMPILE_OPTIONS "-w;-Wno-error"
)
