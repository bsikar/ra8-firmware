#
# Per-app CMake for the `tz_nsc_cgc_usb` firmware.
#
# Variant of the usb_cdc_echo demo that proves the NSC-veneer fix for
# CGC-from-NS silent failure. TrustZone is forced ON; the NS-side
# main() requests CGC bring-up (PLL2, USBCKCR/USBCKDIVCR, clock
# query) via Non-Secure Callable veneers in libs/ra_nsc/ra_nsc_cgc.c
# rather than touching the always-Secure CGC register block directly.
#
# Standalone:
#   cmake -S . -B build \
#         -DCMAKE_TOOLCHAIN_FILE=../../../cmake/toolchain-ra8d2.cmake \
#         -DRA_USE_THREADX=ON -DRA_USE_USBX=ON
#   cmake --build build
#
# Top-level builds use the same CMakeLists.txt by going through
# add_subdirectory.
#
# Note: this app requires both `RA_USE_THREADX=ON` and `RA_USE_USBX=ON`.
# Without both, the build is skipped (so the top-level cmake -B build
# keeps working in the default bare-metal-only configuration).
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

cmake_minimum_required(VERSION 3.20)

get_directory_property(_ra_has_parent PARENT_DIRECTORY)
set(RA_APP_STANDALONE FALSE)
if(NOT _ra_has_parent)
    set(RA_APP_STANDALONE TRUE)
endif()

if(RA_APP_STANDALONE)
    get_filename_component(RA_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

    file(READ "${RA_REPO_ROOT}/VERSION" RA_PROJECT_VERSION)
    string(STRIP "${RA_PROJECT_VERSION}" RA_PROJECT_VERSION)

    project(tz_nsc_cgc_usb
        LANGUAGES C ASM
        VERSION ${RA_PROJECT_VERSION}
        DESCRIPTION "TrustZone NSC-veneer CGC + USB-FS demo for RA8D2"
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

    # TrustZone is FORCED ON for this demo: the whole point is to
    # exercise the CGC NSC veneers from a Non-Secure caller.
    set(RA_TRUSTZONE_ENABLE ON CACHE BOOL "Enable Cortex-M85 TrustZone (-mcmse + SAU)" FORCE)
    option(RA_USE_THREADX      "Enable the vendored ThreadX RTOS"       ON)
    option(RA_USE_USBX         "Enable the vendored USBX USB stack"     ON)

    include(${RA_REPO_ROOT}/cmake/threadx.cmake OPTIONAL RESULT_VARIABLE _tx_inc)
    include(${RA_REPO_ROOT}/cmake/usbx.cmake)
else()
    get_filename_component(RA_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
endif()

if(NOT RA_USE_USBX OR NOT RA_USE_THREADX)
    message(STATUS "tz_nsc_cgc_usb: skipped (RA_USE_USBX=${RA_USE_USBX}, RA_USE_THREADX=${RA_USE_THREADX})")
    return()
endif()

# Top-level builds discover this app even when RA_TRUSTZONE_ENABLE is
# OFF in the parent project. Force it ON for THIS target only -- the
# whole demo is meaningless without -mcmse.
set(RA_TRUSTZONE_ENABLE ON)

include(${RA_REPO_ROOT}/cmake/ra_warnings.cmake)

#############################################################################
# Sources
#############################################################################

set(RA_APP_NAME tz_nsc_cgc_usb)

set(APP_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/vector_table.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system_init.c
    ${CMAKE_CURRENT_SOURCE_DIR}/secure_exception.c
    ${CMAKE_CURRENT_SOURCE_DIR}/trustzone_init.c
)

file(GLOB_RECURSE LIB_RA_CORE_SOURCES   CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_core/src/*.c)
file(GLOB_RECURSE LIB_RA_HAL_SOURCES    CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_hal/src/*.c)
file(GLOB_RECURSE LIB_RA_NET_PAL_SOURCES CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_net_pal/src/*.c)
file(GLOB_RECURSE LIB_RA_USB_PAL_SOURCES CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_usb_pal/src/*.c)
file(GLOB_RECURSE LIB_RA_NSC_SOURCES    CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_nsc/src/*.c)
# ra_nsc_comms.c declares an SPI veneer with 5 args (channel + tx + rx +
# len + bit_width). Under -mcmse the trailing argument spills onto the
# stack, which the cmse_nonsecure_entry attribute forbids. The CGC
# demo does not need any of the comms veneers, so trim the file out
# of this app's TU set rather than refactoring the SPI signature on
# the side. Same applies to ra_nsc_eth.c (multi-arg send/recv).
list(FILTER LIB_RA_NSC_SOURCES EXCLUDE REGEX "ra_nsc_comms\\.c$")
list(FILTER LIB_RA_NSC_SOURCES EXCLUDE REGEX "ra_nsc_eth\\.c$")
file(GLOB_RECURSE LIB_RA_BOARD_SOURCES  CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_board_ek_ra8d2/src/*.c)
file(GLOB_RECURSE APP_SECURE_SOURCES    CONFIGURE_DEPENDS ${RA_REPO_ROOT}/src/secure_app/*.c)

# Pull in the USBX bridge sources via the global property that
# port/usbx/CMakeLists.txt populates.
get_property(RA_USBX_PORT_SOURCES GLOBAL PROPERTY RA_USBX_PORT_SOURCES)

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
    ${APP_SECURE_SOURCES}
    ${RA_USBX_PORT_SOURCES}
)

ra_target_enable_project_warnings(${RA_APP_NAME}.elf STACK_USAGE_BYTES 2200)
target_compile_options(${RA_APP_NAME}.elf PRIVATE -fshort-enums)

# ThreadX/USBX vendor headers and APIs trip several strict-warning
# gates we apply to first-party code (CHAR* name parameters, redundant
# declarations, casts, redefined macros). Relax the gate for this app
# only; the rest of the codebase still gets the full -Werror set.
target_compile_options(${RA_APP_NAME}.elf PRIVATE
    -Wno-error=discarded-qualifiers
    -Wno-error=cast-qual
    -Wno-error=cast-align
    -Wno-error=redundant-decls
    -Wno-error=missing-prototypes
    -Wno-error=builtin-macro-redefined
    -Wno-error
)

# TrustZone is mandatory for this demo (forced ON above).
target_compile_definitions(${RA_APP_NAME}.elf PRIVATE RA_TRUSTZONE_ENABLE)
target_compile_options(${RA_APP_NAME}.elf PRIVATE -mcmse)
target_link_options(${RA_APP_NAME}.elf PRIVATE -mcmse)

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
)

# USBX (and ThreadX, transitively) come in as INTERFACE libraries.
target_link_libraries(${RA_APP_NAME}.elf PRIVATE
    usbx
    usbx_port_ra_usb
)

target_link_options(${RA_APP_NAME}.elf PRIVATE
    -T${LINKER_SCRIPT}
    -Wl,--Map=${RA_APP_NAME}.map
)

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
