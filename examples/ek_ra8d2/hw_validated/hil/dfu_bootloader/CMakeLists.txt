# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMakeLists for dfu_bootloader (immutable USB-DFU MRAM bootloader).
#
# The first 128 KiB of code-MRAM. At reset it boots the valid application slot
# (software A/B select + jump) or, when no slot is valid (or a trigger is set),
# brings up the USBX DFU device on USB-FS and programs the inactive slot. Uses
# the controller-agnostic libs/ra8_dfu core.
#
#
cmake_minimum_required(VERSION 3.20)

# A standalone `cmake -S . -B build` makes this file the top-level
# listfile, which CMake requires to contain a literal, direct project()
# call. The aggregate repo-root build instead pulls this app in via
# add_subdirectory() (project() is already called at the root), so guard on
# PARENT_DIRECTORY to avoid a redundant nested sub-project. ra8_add_app()
# below re-runs project() in the standalone case to apply the shared VERSION
# and the app description.
get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(dfu_bootloader LANGUAGES C ASM)
endif()
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")
ra8_add_app(
  NAME dfu_bootloader
  STACK_BYTES 32768
  DESCRIPTION
    "Immutable USB-DFU MRAM bootloader: A/B slot boot + jump, or DFU the inactive slot over USB-FS"
  USES threadx usbx
  LIBS ra8_dfu
  EXTRA_SRCS ${_d}/libs/ra8_psa_crypto/src/ra8_psa_crypto.c
)

# --- Root of Trust (RA8_ENABLE_ROOT_OF_TRUST): authenticate every launched slot --
# The copy-to-run launch gate (ra8_dfu_launch) verifies the slot's ECDSA-P256 +
# SHA-256 signature and the extra-MRAM anti-rollback counter BEFORE copying the
# image to the SRAM run base -- default-deny on any failure. ra8_rot,
# ra8_dfu_launch, and ra8_dfu_antirollback are already globbed in via `LIBS ra8_dfu`,
# and ra8_rsip (software SHA-256) via ra8_hal; this block links the real
# tf-psa-crypto backend (the exact recipe secure_boot_hil uses to prove the
# mechanism on silicon) and turns the gate on. The bootloader's src/psa_verify_rng.c
# provides a FAIL-CLOSED external-RNG hook (verify draws no randomness).
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
add_library(tfpsa_dfu STATIC ${_TF_SRC})
target_include_directories(
  tfpsa_dfu SYSTEM
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
  tfpsa_dfu
  PUBLIC MBEDTLS_CONFIG_FILE="mbedtls_config.h" TF_PSA_CRYPTO_CONFIG_FILE="tf_psa_crypto_config.h"
         MBEDTLS_PLATFORM_MEMORY MBEDTLS_MEMORY_BUFFER_ALLOC_C
)
# No warning suppression: this OBJECT library has no warning profile to suppress.
# ra8_add_app() applies the project profile to the app target, not to this
# separately-declared tf-psa-crypto object library, so its compile line is the
# toolchain flags plus this one. Compiling all 77 tf-psa-crypto TUs with
# -Wno-attributes and -Wno-redundant-decls removed, under arm-none-eabi-gcc
# 13.3.1 at -O0, emits nothing at all: neither -Wattributes nor
# -Wredundant-decls is ever enabled here. Both are deleted; -fno-strict-aliasing
# stays because it changes code generation, not diagnostics.
target_compile_options(tfpsa_dfu PRIVATE -fno-strict-aliasing)

target_link_libraries(dfu_bootloader.elf PRIVATE tfpsa_dfu)
target_compile_definitions(dfu_bootloader.elf PRIVATE RA8_ENABLE_ROOT_OF_TRUST)
target_include_directories(
  dfu_bootloader.elf PRIVATE ${_d}/libs/ra8_psa_crypto/inc ${_d}/libs/ra8_dfu/inc
)

# The tf-psa include directories are SYSTEM, so its header diagnostics do not
# weaken the full project warning profile on the first-party app and RNG shim.
