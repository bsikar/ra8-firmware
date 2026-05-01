#
# Per-app CMake for the `threadx_https_client` firmware.
#
# Brings up SCI8 + RMII + ThreadX + NetX Duo + Mbed TLS, opens a TCP
# socket to www.example.com:443, runs an Mbed TLS handshake (with the
# AES + SHA-256 primitives routed through ra_rsip via the
# port/mbedtls/ shims), GETs ``/``, and dumps the first 1 KiB of the
# response body to SCI8.
#
# Standalone:
#   cmake -S . -B build \
#         -DCMAKE_TOOLCHAIN_FILE=../../cmake/toolchain-ra8d2.cmake \
#         -DRA_USE_THREADX=ON -DRA_USE_NETXDUO=ON -DRA_USE_MBEDTLS=ON
#   cmake --build build
#
# Pulled in by the top-level CMakeLists.txt the same way.
#
# Note: this app requires all three of `RA_USE_THREADX=ON`,
# `RA_USE_NETXDUO=ON`, and `RA_USE_MBEDTLS=ON`. Without any one of
# them, the build is skipped (so the top-level `cmake -B build` keeps
# working in the default bare-metal-only configuration).
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

    project(threadx_https_client
        LANGUAGES C ASM
        VERSION ${RA_PROJECT_VERSION}
        DESCRIPTION "ThreadX + NetX Duo + Mbed TLS HTTPS client demo for RA8D2"
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
    option(RA_USE_THREADX      "Enable the vendored ThreadX RTOS"            ON)
    option(RA_USE_NETXDUO      "Enable the vendored NetX Duo TCP/IP stack"   ON)
    option(RA_USE_MBEDTLS      "Enable the vendored Mbed TLS library"        ON)

    include(${RA_REPO_ROOT}/cmake/threadx.cmake OPTIONAL RESULT_VARIABLE _tx_inc)
    include(${RA_REPO_ROOT}/cmake/netxduo.cmake)
    include(${RA_REPO_ROOT}/cmake/mbedtls.cmake)
else()
    get_filename_component(RA_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
    # The top-level build does NOT enable RA_USE_MBEDTLS by default,
    # so when invoked from `cmake -B build` at the repo root with the
    # default options this app simply gets skipped below.
    include(${RA_REPO_ROOT}/cmake/mbedtls.cmake)
endif()

# Skip the executable when any of our three required dependencies is
# not enabled. The top-level build still walks examples/* by default,
# and we don't want to drag the vendored trees into the bare-metal-only
# configuration.
if(NOT RA_USE_NETXDUO OR NOT RA_USE_THREADX OR NOT RA_USE_MBEDTLS)
    message(STATUS
        "threadx_https_client: skipped "
        "(RA_USE_THREADX=${RA_USE_THREADX}, "
        "RA_USE_NETXDUO=${RA_USE_NETXDUO}, "
        "RA_USE_MBEDTLS=${RA_USE_MBEDTLS})")
    return()
endif()

include(${RA_REPO_ROOT}/cmake/ra_warnings.cmake)

#############################################################################
# Sources
#############################################################################

set(RA_APP_NAME threadx_https_client)

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
file(GLOB_RECURSE APP_SECURE_SOURCES     CONFIGURE_DEPENDS ${RA_REPO_ROOT}/src/secure_app/*.c)

# Pull in port shim sources via the global properties their CMakeLists
# populate. Each is empty when its option is OFF; the `if` above guards
# against that.
get_property(RA_NETXDUO_PORT_SOURCES GLOBAL PROPERTY RA_NETXDUO_PORT_SOURCES)
get_property(RA_MBEDTLS_PORT_SOURCES GLOBAL PROPERTY RA_MBEDTLS_PORT_SOURCES)

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
    ${APP_SECURE_SOURCES}
    ${RA_NETXDUO_PORT_SOURCES}
    ${RA_MBEDTLS_PORT_SOURCES}
)

ra_target_enable_project_warnings(${RA_APP_NAME}.elf STACK_USAGE_BYTES 4096)
target_compile_options(${RA_APP_NAME}.elf PRIVATE -fshort-enums)

# ThreadX/FileX/NetX/USBX/GUIX vendor APIs take CHAR* (non-const) for name parameters
# while we pass C string literals. Relax the strict-warning gate for this app only;
# the rest of the codebase still gets -Werror.
# ThreadX/FileX/NetX/USBX/GUIX/MbedTLS vendor headers and APIs trip several
# strict-warning gates we apply to first-party code (CHAR* name parameters,
# redundant declarations, casts, redefined macros). Relax the gate for these
# apps only; the rest of the codebase still gets the full -Werror set.
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
)

# NetX Duo + Mbed TLS (and ThreadX, transitively) come in as INTERFACE libraries.
# Mbed TLS lists its include dir with the port/ shim path PREPENDED so the
# AES_ALT / SHA256_ALT inclusion sites resolve here.
target_link_libraries(${RA_APP_NAME}.elf PRIVATE
    netxduo
    netxduo_port_ra_eth
    mbedtls
    mbedtls_port_ra_rsip
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
