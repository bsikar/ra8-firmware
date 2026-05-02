#
# Per-app CMake for the `threadx_lwip_tcp_echo` firmware.
#
# Brings up SCI8 + RMII + ThreadX + lwIP on top of our hardware
# Ethernet driver shim (`port/lwip/netif/ra_etha_netif.c`), listens
# on TCP/7 (echo protocol), and logs every echo to SCI8.
#
# Standalone:
#   cmake -S . -B build \
#         -DCMAKE_TOOLCHAIN_FILE=../../cmake/toolchain-ra8d2.cmake \
#         -DRA_USE_THREADX=ON -DRA_USE_LWIP=ON
#   cmake --build build
#
# Pulled in by the top-level CMakeLists.txt the same way.
#
# Note: this app requires both `RA_USE_THREADX=ON` and
# `RA_USE_LWIP=ON`. Without both, the build is skipped (so the
# top-level `cmake -B build` keeps working in the default
# bare-metal-only configuration).
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
    get_filename_component(RA_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

    file(READ "${RA_REPO_ROOT}/VERSION" RA_PROJECT_VERSION)
    string(STRIP "${RA_PROJECT_VERSION}" RA_PROJECT_VERSION)

    project(threadx_lwip_tcp_echo
        LANGUAGES C ASM
        VERSION ${RA_PROJECT_VERSION}
        DESCRIPTION "ThreadX + lwIP TCP echo demo for RA8D2"
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

    option(RA_TRUSTZONE_ENABLE "Enable Cortex-M85 TrustZone (-mcmse + SAU)" OFF)
    option(RA_USE_THREADX      "Enable the vendored ThreadX RTOS" ON)
    option(RA_USE_LWIP         "Enable the vendored lwIP TCP/IP stack" ON)

    include(${RA_REPO_ROOT}/cmake/threadx.cmake OPTIONAL RESULT_VARIABLE _tx_inc)
else()
    get_filename_component(RA_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

    # When built from the top-level CMakeLists, RA_USE_LWIP is OFF by
    # default. Force it ON locally so the lwIP static library target
    # exists for this app while leaving the bare-metal-only top-level
    # configure unaffected.
    if(NOT DEFINED RA_USE_LWIP)
        set(RA_USE_LWIP ON)
    endif()
endif()

# Skip the executable when ThreadX or lwIP is not enabled. The
# top-level build still walks examples/* by default; we don't want
# to drag the vendored tree into the bare-metal-only configuration.
if(NOT RA_USE_THREADX OR NOT RA_USE_LWIP)
    message(STATUS "threadx_lwip_tcp_echo: skipped (RA_USE_THREADX=${RA_USE_THREADX}, RA_USE_LWIP=${RA_USE_LWIP})")
    return()
endif()

# port/lwip/CMakeLists.txt is self-contained. Pull it in once; the
# guard inside short-circuits on subsequent includes.
if(NOT TARGET lwip)
    add_subdirectory(${RA_REPO_ROOT}/port/lwip
                     ${CMAKE_BINARY_DIR}/_port_lwip)
endif()

include(${RA_REPO_ROOT}/cmake/ra_warnings.cmake)

#############################################################################
# Sources
#############################################################################

set(RA_APP_NAME threadx_lwip_tcp_echo)

set(APP_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/vector_table.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system_init.c
    ${CMAKE_CURRENT_SOURCE_DIR}/secure_exception.c
    ${CMAKE_CURRENT_SOURCE_DIR}/trustzone_init.c
)

file(GLOB_RECURSE LIB_RA_CORE_SOURCES    CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_core/src/*.c)
file(GLOB_RECURSE LIB_RA_HAL_SOURCES     CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_hal/src/*.c)
file(GLOB_RECURSE LIB_RA_NET_PAL_SOURCES CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_net_pal/src/*.c)
file(GLOB_RECURSE LIB_RA_USB_PAL_SOURCES CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_usb_pal/src/*.c)
file(GLOB_RECURSE LIB_RA_NSC_SOURCES     CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_nsc/src/*.c)
file(GLOB_RECURSE LIB_RA_BOARD_SOURCES   CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_board_ek_ra8d2/src/*.c)
file(GLOB_RECURSE APP_SECURE_SOURCES     CONFIGURE_DEPENDS ${RA_REPO_ROOT}/src/secure_app/*.c)

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
)

ra_target_enable_project_warnings(${RA_APP_NAME}.elf STACK_USAGE_BYTES 2200)
target_compile_options(${RA_APP_NAME}.elf PRIVATE -fshort-enums)

# ThreadX/FileX/NetX/USBX/GUIX/MbedTLS/lwIP vendor headers and APIs trip
# several strict-warning gates we apply to first-party code (CHAR* name
# parameters, redundant declarations, casts, redefined macros). Relax
# the gate for this app only; the rest of the codebase still gets the
# full -Werror set.
target_compile_options(${RA_APP_NAME}.elf PRIVATE
    -Wno-error=discarded-qualifiers
    -Wno-error=cast-qual
    -Wno-error=cast-align
    -Wno-error=redundant-decls
    -Wno-error=missing-prototypes
    -Wno-error=builtin-macro-redefined
    -Wno-error
)

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
)

# lwIP (and ThreadX, transitively) come in as a static library. The
# include directories for lwipopts.h, the lwIP src tree, and the
# ra_etha_netif glue all flow through PUBLIC properties on the
# `lwip` target.
target_link_libraries(${RA_APP_NAME}.elf PRIVATE
    lwip
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
