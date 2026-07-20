#
# Per-app CMake for `secure_boot_ns_hil` -- the TrustZone BLXNS root-of-trust
# proof (#172). A TWO-PROJECT TrustZone build:
#
#   * Secure ELF (ra8_add_app): SAU + SRAM-NS boundary + NS-image copy
#     (trustzone_init.c), then main() runs the RA8_ENABLE_ROOT_OF_TRUST verify
#     (ra8_rot + ra8_psa_crypto + tf-psa-crypto) and BLXNS-es via
#     ra8_tz_secure_boot_jump_ns -- default-deny on a failed signature.
#   * Non-Secure ELF (secure_boot_ns_hil_ns.elf): a minimal liveness beacon
#     (ns_main.c) placed at the NS addresses by ns_image.ld, whose .ns_rot_header
#     tells the Secure verifier the signed-body length.
#
# The NS image is SIGNED (tools/rot_sign.py) and merged with the Secure hex into
# two flashable artifacts -- a genuine one (NS runs) and a one-byte-tampered one
# (NS denied). Signing degrades gracefully when the RoT key is absent (see
# sign_and_merge.py). Copyright (c) 2026 Brighton Sikarskie.
# SPDX-License-Identifier: MIT
#
cmake_minimum_required(VERSION 3.20)

get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(secure_boot_ns_hil LANGUAGES C ASM)
endif()

set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

get_directory_property(_ra8_p PARENT_DIRECTORY)
if(NOT _ra8_p)
  option(RA8_TRUSTZONE_ENABLE "Enable Cortex-M85 TrustZone (-mcmse + SAU)" ON)
endif()

# ---- Secure ELF ----------------------------------------------------------
# ra8_tz_secure_boot (LIBS, globbed in so RA8_ENABLE_ROOT_OF_TRUST reaches the
# BLXNS gate internal_ns_verify_or_deny). ra8_rot + ra8_psa_crypto + the
# fail-closed RNG shim via EXTRA_SRCS; ra8_rsip (software SHA) auto-globbed.
ra8_add_app(
  NAME secure_boot_ns_hil
  STACK_BYTES 32768
  DESCRIPTION "TrustZone BLXNS root-of-trust proof (Secure verify + NS liveness)"
  LIBS ra8_tz_secure_boot
  EXTRA_SRCS ${_d}/libs/ra8_psa_crypto/src/ra8_psa_crypto.c ${_d}/libs/ra8_dfu/src/ra8_rot.c
             ${CMAKE_CURRENT_SOURCE_DIR}/src/psa_verify_rng.c
)

# tf-psa-crypto software crypto for ARM (same recipe as rot_verify_hil /
# secure_boot_hil): SYSTEM includes keep the vendored psa headers out of
# -Werror; the port config is software-only with EXTERNAL_RNG; a static-buffer
# allocator backs mbedtls_calloc (no libc heap on target).
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
add_library(tfpsa_sbns STATIC ${_TF_SRC})
target_include_directories(
  tfpsa_sbns SYSTEM
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
  tfpsa_sbns
  PUBLIC MBEDTLS_CONFIG_FILE="mbedtls_config.h" TF_PSA_CRYPTO_CONFIG_FILE="tf_psa_crypto_config.h"
         MBEDTLS_PLATFORM_MEMORY MBEDTLS_MEMORY_BUFFER_ALLOC_C
)
target_compile_options(tfpsa_sbns PRIVATE -w -Wno-error -fno-strict-aliasing)

target_link_libraries(secure_boot_ns_hil.elf PRIVATE tfpsa_sbns)
target_compile_definitions(
  secure_boot_ns_hil.elf PRIVATE RA8_ENABLE_ROOT_OF_TRUST RA8_TRUSTZONE_ENABLE
)
target_include_directories(
  secure_boot_ns_hil.elf PRIVATE ${_d}/libs/ra8_psa_crypto/inc ${_d}/libs/ra8_dfu/inc
                                 ${_d}/libs/ra8_tz_secure_boot/inc
)

# main.c + the RNG shim include the vendored psa/crypto.h (not -Werror-clean via
# a non-SYSTEM path in a couple of spots); compile the psa-touching glue TUs
# -w -Wno-error while the rest of the app keeps -Werror.
set_source_files_properties(
  ${CMAKE_CURRENT_SOURCE_DIR}/main.c ${CMAKE_CURRENT_SOURCE_DIR}/src/psa_verify_rng.c
  PROPERTIES COMPILE_OPTIONS "-w;-Wno-error"
)

# ---- Non-Secure ELF ------------------------------------------------------
# A standalone minimal image: no libc/RTOS/veneers. Same toolchain ABI as the
# Secure side (cpu flags from the toolchain, C23 global), but no startup files
# (the Secure boot copies it and ns_reset_handler is the entry) and its own
# NS-image linker script.
add_executable(secure_boot_ns_hil_ns.elf ${CMAKE_CURRENT_SOURCE_DIR}/ns_main.c)
target_compile_options(secure_boot_ns_hil_ns.elf PRIVATE -fshort-enums -ffreestanding)
# Raw add_executable targets skip the ra8_add_app warning profile, so without
# this the NS image compiles WITHOUT -Wall/-Wextra/-Werror (the T2-05
# warnings-scope hole).
ra8_target_enable_project_warnings(secure_boot_ns_hil_ns.elf STACK_USAGE_BYTES 32768)
target_link_options(
  secure_boot_ns_hil_ns.elf
  PRIVATE
  -nostartfiles
  -T${CMAKE_CURRENT_SOURCE_DIR}/ns_image.ld
  -Wl,--Map=secure_boot_ns_hil_ns.map
)
set_target_properties(
  secure_boot_ns_hil_ns.elf PROPERTIES LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ns_image.ld"
)
# The Secure hex is an input to the merge below, so build the Secure ELF first.
add_dependencies(secure_boot_ns_hil_ns.elf secure_boot_ns_hil.elf)

# ---- Sign the NS image + merge Secure+NS -> genuine + tampered hex --------
# Objcopy the Secure ELF to ihex (stripping the option-setting records the
# J-Link/MRAM flasher cannot write), then sign_and_merge.py signs the NS body
# (rot_sign.py) and merges. Genuine overwrites secure_boot_ns_hil.hex so the
# existing flash flow picks it up; the tampered variant is a second artifact.
# RA8_ROT_KEY (or ~/ra8d2-rot-signing-key.pem) selects the signing key; when
# absent the step degrades gracefully and prints the manual sign command.
if(DEFINED ENV{RA8_ROT_KEY})
  set(_sbns_key "$ENV{RA8_ROT_KEY}")
else()
  set(_sbns_key "$ENV{HOME}/ra8d2-rot-signing-key.pem")
endif()

add_custom_command(
  TARGET secure_boot_ns_hil_ns.elf
  POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} --remove-section=.option_setting* -O ihex
          $<TARGET_FILE:secure_boot_ns_hil.elf> secure_boot_ns_hil_secure.hex
  COMMAND
    python3 ${CMAKE_CURRENT_SOURCE_DIR}/sign_and_merge.py --secure-hex secure_boot_ns_hil_secure.hex
    --ns-elf $<TARGET_FILE:secure_boot_ns_hil_ns.elf> --objcopy ${CMAKE_OBJCOPY} --rot-sign
    ${_d}/tools/rot_sign.py --merge ${_d}/scripts/utils/merge_ihex.py --out-genuine
    secure_boot_ns_hil.hex --out-tampered secure_boot_ns_hil_tampered.hex --key ${_sbns_key}
  VERBATIM
  COMMENT "Signing NS image + merging Secure+NS -> secure_boot_ns_hil.hex (+_tampered)"
)
