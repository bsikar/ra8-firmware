# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMake stub for `cpu1_pingpong_ipc`. The Cortex-M85 (CPU0) image is
# built by the shared cmake/ra8_add_app.cmake recipe; the Cortex-M33 (CPU1)
# image is a second executable objcopy'd into the M85 ELF.
#
# This app exercises the FSP-style secure-boot scaffolding (SAU init +
# CPSCU PRCR-unlocked IPCSAR write + BLXNS into the NS world) from
# libs/ra8_tz_secure_boot/. RA8_TRUSTZONE_ENABLE defaults ON for standalone
# builds because the IPCSAR write is secure-only. The ra8_nsc sources are
# excluded (NO_NSC): several existing NSC veneers take struct-by-pointer
# arguments that gcc rejects under -mcmse, and this app uses none of them.
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
  project(cpu1_pingpong_ipc LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

# Secure-only IPCSAR write: default TrustZone ON for the standalone build.
get_directory_property(_ra8_p PARENT_DIRECTORY)
if(NOT _ra8_p)
  option(RA8_TRUSTZONE_ENABLE "Enable Cortex-M85 TrustZone (-mcmse + SAU)" ON)
endif()

ra8_add_app(
  NAME cpu1_pingpong_ipc
  STACK_BYTES 2200
  DESCRIPTION "Bare-metal cpu1_pingpong_ipc firmware for RA8D2" AUX_SRCS src/cpu1_main.c
  NO_NSC
  LIBS ra8_tz_secure_boot
)

#############################################################################
# Second (Cortex-M33 / CPU1) image, embedded into the M85 ELF
#############################################################################
if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_SYSTEM_NAME STREQUAL "Generic")
  set(CPU1_LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/linker_script_cpu1.ld)
  set(CPU1_NAME cpu1_pingpong_ipc_cpu1)
  add_executable(
    ${CPU1_NAME}.elf
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cpu1_main.c ${RA8_REPO_ROOT}/libs/ra8_hal/src/ra8_ipc.c
    ${RA8_REPO_ROOT}/libs/ra8_core/src/ra8_log.c ${RA8_REPO_ROOT}/libs/ra8_core/src/ra8_scb.c
  )
  target_compile_definitions(${CPU1_NAME}.elf PRIVATE RA8_BUILD_FOR_CPU1)
  target_compile_options(
    ${CPU1_NAME}.elf
    PRIVATE -mcpu=cortex-m33
            -mthumb
            -mfloat-abi=hard
            -mfpu=fpv5-sp-d16
            -ffreestanding
            -fno-builtin
            -fshort-enums
            -Os
            -g3
  )
  target_link_options(
    ${CPU1_NAME}.elf
    PRIVATE
    -mcpu=cortex-m33
    -mthumb
    -mfloat-abi=hard
    -mfpu=fpv5-sp-d16
    -nostartfiles
    -T${CPU1_LINKER_SCRIPT}
    -Wl,--Map=${CPU1_NAME}.map
  )
  target_include_directories(
    ${CPU1_NAME}.elf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc ${CMAKE_CURRENT_SOURCE_DIR}/src
                             ${RA8_REPO_ROOT}/libs/ra8_core/inc ${RA8_REPO_ROOT}/libs/ra8_hal/inc
  )
  set_target_properties(${CPU1_NAME}.elf PROPERTIES LINK_DEPENDS ${CPU1_LINKER_SCRIPT})
  add_custom_command(
    TARGET ${CPU1_NAME}.elf
    POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${CPU1_NAME}.elf> ${CPU1_NAME}.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${CPU1_NAME}.elf> ${CPU1_NAME}.bin
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${CPU1_NAME}.elf>
    COMMENT "Emitting ${CPU1_NAME}.hex / .bin and reporting its size"
  )

  # Pack the CPU1 .bin as a relocatable .cpu1_image object and link it into
  # the M85 ELF; linker_script.ld pins .cpu1_image at ORIGIN(MRAM_CPU1) so
  # one .hex covers MRAM (0x02000000) and MRAM_CPU1 (0x020C0000).
  set(CPU1_BIN ${CMAKE_CURRENT_BINARY_DIR}/${CPU1_NAME}.bin)
  set(CPU1_BLOB_OBJ ${CMAKE_CURRENT_BINARY_DIR}/${CPU1_NAME}_blob.o)
  add_custom_command(
    OUTPUT ${CPU1_BLOB_OBJ}
    COMMAND ${CMAKE_OBJCOPY} -I binary -O elf32-littlearm -B arm --rename-section
            .data=.cpu1_image,alloc,load,readonly,contents ${CPU1_BIN} ${CPU1_BLOB_OBJ}
    DEPENDS ${CPU1_NAME}.elf
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Packing ${CPU1_NAME}.bin as relocatable .cpu1_image object"
  )
  target_sources(cpu1_pingpong_ipc.elf PRIVATE ${CPU1_BLOB_OBJ})
  set_source_files_properties(${CPU1_BLOB_OBJ} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
  add_dependencies(cpu1_pingpong_ipc.elf ${CPU1_NAME}.elf)
endif()
