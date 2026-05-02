#
# Per-app CMake for the `threadx_guix_demo` firmware.
#
# Brings up GLCDC + ThreadX + GUIX, then renders a tiny window with a
# label ("Hello from GUIX on RA8D2") and an animated colour-cycling
# accent rectangle that stands in for a button hit. The build pulls in
# both the vendored Eclipse ThreadX RTOS and the vendored Eclipse GUIX
# UI framework via cmake/threadx.cmake and cmake/guix.cmake.
#
# Standalone:
#   cmake -S . -B build \
#         -DCMAKE_TOOLCHAIN_FILE=../../../cmake/toolchain-ra8d2.cmake \
#         -DRA_USE_THREADX=ON -DRA_USE_GUIX=ON
#   cmake --build build
#
# Pulled in by the top-level CMakeLists.txt the same way -- the demo
# silently disables itself when neither RA_USE_THREADX nor RA_USE_GUIX
# is requested so the default `cmake -B build` keeps building blink/.
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

    project(threadx_guix_demo
        LANGUAGES C ASM
        VERSION ${RA_PROJECT_VERSION}
        DESCRIPTION "ThreadX + GUIX hello-world demo for RA8D2"
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
    option(RA_USE_GUIX         "Enable the vendored GUIX UI framework" ON)

    include(${RA_REPO_ROOT}/cmake/threadx.cmake OPTIONAL RESULT_VARIABLE _tx_inc)
    include(${RA_REPO_ROOT}/cmake/guix.cmake)
else()
    get_filename_component(RA_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
endif()

# Skip the executable when GUIX is not enabled. The top-level build
# walks examples/* by default, and we don't want to drag the vendored
# GUIX tree into the bare-metal-only configuration.
if(NOT RA_USE_GUIX OR NOT RA_USE_THREADX)
    message(STATUS "threadx_guix_demo: skipped (RA_USE_GUIX=${RA_USE_GUIX}, RA_USE_THREADX=${RA_USE_THREADX})")
    return()
endif()

include(${RA_REPO_ROOT}/cmake/ra_warnings.cmake)

#############################################################################
# Sources
#############################################################################

set(RA_APP_NAME threadx_guix_demo)

set(APP_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/vector_table.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system_init.c
    ${CMAKE_CURRENT_SOURCE_DIR}/secure_exception.c
    ${CMAKE_CURRENT_SOURCE_DIR}/trustzone_init.c
)

file(GLOB_RECURSE LIB_RA_CORE_SOURCES    CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_core/src/*.c)
file(GLOB_RECURSE LIB_RA_HAL_SOURCES     CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_hal/src/*.c)
file(GLOB_RECURSE LIB_RA_GFX_SOURCES     CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_gfx/src/*.c)
file(GLOB_RECURSE LIB_RA_NET_PAL_SOURCES CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_net_pal/src/*.c)
file(GLOB_RECURSE LIB_RA_USB_PAL_SOURCES CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_usb_pal/src/*.c)
file(GLOB_RECURSE LIB_RA_NSC_SOURCES     CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_nsc/src/*.c)
file(GLOB_RECURSE LIB_RA_BOARD_SOURCES     CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_board_ek_ra8d2/src/*.c)
file(GLOB_RECURSE APP_SECURE_SOURCES     CONFIGURE_DEPENDS ${RA_REPO_ROOT}/src/secure_app/*.c)

# Pull in the GUIX <-> GLCDC shim source via the property that
# port/guix/CMakeLists.txt populates.
get_property(RA_GUIX_PORT_SOURCES GLOBAL PROPERTY RA_GUIX_PORT_SOURCES)

#############################################################################
# Executable
#############################################################################

set(LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/linker_script.ld)

add_executable(${RA_APP_NAME}.elf
    ${APP_SOURCES}
    ${LIB_RA_CORE_SOURCES}
    ${LIB_RA_HAL_SOURCES}
    ${LIB_RA_GFX_SOURCES}
    ${LIB_RA_NET_PAL_SOURCES}
    ${LIB_RA_USB_PAL_SOURCES}
    ${LIB_RA_NSC_SOURCES}
    ${LIB_RA_BOARD_SOURCES}
    ${APP_SECURE_SOURCES}
    ${RA_GUIX_PORT_SOURCES}
)

ra_target_enable_project_warnings(${RA_APP_NAME}.elf STACK_USAGE_BYTES 4400)
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
    ${RA_REPO_ROOT}/libs/ra_gfx/inc
    ${RA_REPO_ROOT}/libs/ra_net_pal/inc
    ${RA_REPO_ROOT}/libs/ra_usb_pal/inc
    ${RA_REPO_ROOT}/libs/ra_nsc/inc
    ${RA_REPO_ROOT}/libs/ra_board_ek_ra8d2/inc
    ${RA_REPO_ROOT}/port/guix
)

target_compile_definitions(${RA_APP_NAME}.elf PRIVATE
    RA_USE_THREADX
    RA_USE_GUIX
)

# GUIX (and ThreadX, transitively) come in as INTERFACE libraries.
target_link_libraries(${RA_APP_NAME}.elf PRIVATE
    guix
    guix_port_ra_glcdc
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
