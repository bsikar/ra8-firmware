#
# Per-app CMake for the `cpu1_pingpong_ipc_ipc` firmware.
#
# Usable two ways:
#   1. Standalone, from inside `cpu1_pingpong_ipc_ipc/`:
#        cmake -S . -B build \
#              -DCMAKE_TOOLCHAIN_FILE=../../../../../cmake/toolchain-ra8d2.cmake
#        cmake --build build
#   2. Pulled in by the top-level CMakeLists.txt via auto-discovery.
#
# Either path produces cpu1_pingpong_ipc_ipc.elf / .hex / .bin in the build dir.
#
# This app exercises the FSP-style secure-boot scaffolding (SAU init +
# CPSCU PRCR-unlocked IPCSAR write + BLXNS into the NS world). The
# secure-boot library is libs/ra_tz_secure_boot/.
#
# RA_TRUSTZONE_ENABLE defaults to ON for this app because the IPCSAR
# write is secure-only -- the single-world build cannot land it.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

cmake_minimum_required(VERSION 3.20)

# Detect standalone invocation. When this file is the top-level project,
# we set up everything (project, version, build type, std). When pulled
# in by the repo root, the parent has already done that.
get_directory_property(_ra_has_parent PARENT_DIRECTORY)
set(RA_APP_STANDALONE FALSE)
if(NOT _ra_has_parent)
    set(RA_APP_STANDALONE TRUE)
endif()

if(RA_APP_STANDALONE)
    # Top-level project mode: replicate just enough of the repo-root cmake
    # to build this single app.
    get_filename_component(RA_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../" ABSOLUTE)

    file(READ "${RA_REPO_ROOT}/VERSION" RA_PROJECT_VERSION)
    string(STRIP "${RA_PROJECT_VERSION}" RA_PROJECT_VERSION)

    project(cpu1_pingpong_ipc
        LANGUAGES C ASM
        VERSION ${RA_PROJECT_VERSION}
        DESCRIPTION "Bare-metal cpu1_pingpong_ipc firmware for RA8D2"
    )

    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    endif()

    set(CMAKE_C_STANDARD 23)
    set(CMAKE_C_STANDARD_REQUIRED ON)
    set(CMAKE_C_EXTENSIONS ON)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

    set(CMAKE_C_FLAGS_DEBUG          "-O0 -g3 -DDEBUG")
    set(CMAKE_ASM_FLAGS_DEBUG        "-g3")
    set(CMAKE_C_FLAGS_RELEASE        "-Os -g1 -DNDEBUG")
    set(CMAKE_ASM_FLAGS_RELEASE      "-g1")
    set(CMAKE_C_FLAGS_RELWITHDEBINFO "-Og -g3")
    set(CMAKE_ASM_FLAGS_RELWITHDEBINFO "-g3")

    # This app *requires* TrustZone -- the IPCSAR write its acceptance
    # criterion depends on is Secure-only. Override the project-wide
    # default by enabling TZ by default for the standalone build.
    option(RA_TRUSTZONE_ENABLE "Enable Cortex-M85 TrustZone (-mcmse + SAU)" ON)
else()
    # Embedded mode: parent already set toolchain, std, build type.
    get_filename_component(RA_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../" ABSOLUTE)
endif()

include(${RA_REPO_ROOT}/cmake/ra_warnings.cmake)

#############################################################################
# Sources
#############################################################################

set(RA_APP_NAME cpu1_pingpong_ipc)

# Per-app sources: main + boot + linker script live next to this file.
set(APP_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/vector_table.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system_init.c
    ${CMAKE_CURRENT_SOURCE_DIR}/secure_exception.c
    ${CMAKE_CURRENT_SOURCE_DIR}/trustzone_init.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_main.c
)

# Shared library + secure-app sources from the repo root.
file(GLOB_RECURSE LIB_RA_CORE_SOURCES   CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_core/src/*.c)
file(GLOB_RECURSE LIB_RA_HAL_SOURCES    CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_hal/src/*.c)
file(GLOB_RECURSE LIB_RA_NET_PAL_SOURCES CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_net_pal/src/*.c)
file(GLOB_RECURSE LIB_RA_USB_PAL_SOURCES CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_usb_pal/src/*.c)
# Drop the full ra_nsc/ source set when building this app with
# ``-mcmse``: several existing NSC veneers (ra_nsc_comms.c, ra_nsc_eth.c,
# etc.) take struct-by-pointer arguments that gcc rejects under
# ``cmse_nonsecure_entry`` (parameters must be register-sized scalars).
# This app's TZ scaffold does not use any of those veneers, so excluding
# the whole library is the cleanest path. Other consumers of ra_nsc are
# untouched because they don't build with -mcmse.
set(LIB_RA_NSC_SOURCES "")
file(GLOB_RECURSE LIB_RA_BOARD_SOURCES  CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_board_ek_ra8d2/src/*.c)
file(GLOB_RECURSE LIB_RA_TZ_SECURE_BOOT_SOURCES CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_tz_secure_boot/src/*.c)
file(GLOB_RECURSE APP_SECURE_SOURCES    CONFIGURE_DEPENDS ${RA_REPO_ROOT}/src/secure_app/*.c)

#############################################################################
# Executable
#############################################################################

set(LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/linker_script.ld)

add_executable(${RA_APP_NAME}.elf
    ${APP_SOURCES}
    ${LIB_RA_CORE_SOURCES}
    ${LIB_RA_HAL_SOURCES}
    ${LIB_RA_NET_PAL_SOURCES}
    ${LIB_RA_USB_PAL_SOURCES}
    ${LIB_RA_NSC_SOURCES}
    ${LIB_RA_BOARD_SOURCES}
    ${LIB_RA_TZ_SECURE_BOOT_SOURCES}
    ${APP_SECURE_SOURCES}
)

ra_target_enable_project_warnings(${RA_APP_NAME}.elf STACK_USAGE_BYTES 2200)
target_compile_options(${RA_APP_NAME}.elf PRIVATE -fshort-enums)

if(RA_TRUSTZONE_ENABLE)
    target_compile_definitions(${RA_APP_NAME}.elf PRIVATE RA_TRUSTZONE_ENABLE)
    target_compile_options(${RA_APP_NAME}.elf PRIVATE -mcmse)
    target_link_options(${RA_APP_NAME}.elf PRIVATE -mcmse)
endif()

target_include_directories(${RA_APP_NAME}.elf PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${RA_REPO_ROOT}/src
    ${RA_REPO_ROOT}/src/inc
    ${RA_REPO_ROOT}/src/secure_app
    ${RA_REPO_ROOT}/libs/ra_core/inc
    ${RA_REPO_ROOT}/libs/ra_hal/inc
    ${RA_REPO_ROOT}/libs/ra_net_pal/inc
    ${RA_REPO_ROOT}/libs/ra_usb_pal/inc
    ${RA_REPO_ROOT}/libs/ra_nsc/inc
    ${RA_REPO_ROOT}/libs/ra_board_ek_ra8d2/inc
    ${RA_REPO_ROOT}/libs/ra_tz_secure_boot/inc
)

target_link_options(${RA_APP_NAME}.elf PRIVATE
    -T${LINKER_SCRIPT}
    -Wl,--Map=${RA_APP_NAME}.map
)

# Re-link if the linker script changes.
set_target_properties(${RA_APP_NAME}.elf PROPERTIES LINK_DEPENDS ${LINKER_SCRIPT})

#############################################################################
# Post-build: .hex / .bin / size dump
#############################################################################

add_custom_command(TARGET ${RA_APP_NAME}.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${RA_APP_NAME}.elf> ${RA_APP_NAME}.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${RA_APP_NAME}.elf> ${RA_APP_NAME}.bin
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${RA_APP_NAME}.elf>
    COMMENT "Generating ${RA_APP_NAME}.hex / .bin and showing size"
)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_SYSTEM_NAME STREQUAL "Generic")
    set(CPU1_LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/linker_script_cpu1.ld)
    set(CPU1_NAME ${RA_APP_NAME}_cpu1)
    add_executable(${CPU1_NAME}.elf
        ${CMAKE_CURRENT_SOURCE_DIR}/cpu1_main.c
        ${RA_REPO_ROOT}/libs/ra_hal/src/ra_ipc.c
        ${RA_REPO_ROOT}/libs/ra_core/src/ra_log.c)
    target_compile_definitions(${CPU1_NAME}.elf PRIVATE RA_BUILD_FOR_CPU1)
    target_compile_options(${CPU1_NAME}.elf PRIVATE
        -mcpu=cortex-m33 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16
        -ffreestanding -fno-builtin -fshort-enums -Os -g3)
    target_link_options(${CPU1_NAME}.elf PRIVATE
        -mcpu=cortex-m33 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16
        -nostartfiles -T${CPU1_LINKER_SCRIPT} -Wl,--Map=${CPU1_NAME}.map)
    target_include_directories(${CPU1_NAME}.elf PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${RA_REPO_ROOT}/libs/ra_core/inc
        ${RA_REPO_ROOT}/libs/ra_hal/inc)
    set_target_properties(${CPU1_NAME}.elf PROPERTIES LINK_DEPENDS ${CPU1_LINKER_SCRIPT})
    add_custom_command(TARGET ${CPU1_NAME}.elf POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${CPU1_NAME}.elf> ${CPU1_NAME}.hex
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${CPU1_NAME}.elf> ${CPU1_NAME}.bin
        COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${CPU1_NAME}.elf>)

    # ------------------------------------------------------------------
    # Embed the CPU1 binary into the M85 ELF so a single SWD flash drops
    # both cores' images on the chip in one shot.
    #
    # Without this step the CPU1 hex was built but never sent to the
    # board (scripts/hil_flash.sh / scripts/flash.sh only loadfile the
    # M85 hex), CPU1 fetched its reset vector from blank MRAM_CPU1
    # (0xFFFFFFFF), and the ping-pong counter g_cpu1_pingpong_ipc_match
    # stayed at 0 for the entire 5 s HIL probe window.
    #
    # arm-none-eabi-objcopy --rename-section .data=.cpu1_image converts
    # the raw .bin into a relocatable ELF object whose only section is
    # named .cpu1_image. The M85 linker_script.ld pins that section to
    # ORIGIN(MRAM_CPU1) so the resulting .hex covers MRAM (0x02000000)
    # AND MRAM_CPU1 (0x020C0000) in one image.
    # ------------------------------------------------------------------
    set(CPU1_BIN ${CMAKE_CURRENT_BINARY_DIR}/${CPU1_NAME}.bin)
    set(CPU1_BLOB_OBJ ${CMAKE_CURRENT_BINARY_DIR}/${CPU1_NAME}_blob.o)
    add_custom_command(OUTPUT ${CPU1_BLOB_OBJ}
        COMMAND ${CMAKE_OBJCOPY}
            -I binary -O elf32-littlearm -B arm
            --rename-section .data=.cpu1_image,alloc,load,readonly,contents
            ${CPU1_BIN} ${CPU1_BLOB_OBJ}
        DEPENDS ${CPU1_NAME}.elf
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Packing ${CPU1_NAME}.bin as relocatable .cpu1_image object")
    target_sources(${RA_APP_NAME}.elf PRIVATE ${CPU1_BLOB_OBJ})
    set_source_files_properties(${CPU1_BLOB_OBJ} PROPERTIES
        EXTERNAL_OBJECT TRUE
        GENERATED TRUE)
    add_dependencies(${RA_APP_NAME}.elf ${CPU1_NAME}.elf)
endif()
