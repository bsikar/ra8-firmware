#
# Per-app CMake for the `ereader` firmware -- the Phase-6 e-reader skeleton.
#
# Pulls in every X-Ware component plus libs/ra_epub (and, when
# present, libs/ra_reflow). Targets the EK-RA8D2 on-board parallel TFT
# panel; the E-Ink panel + custom carrier board are deferred to Phase
# 6.2.1.
#
# Standalone:
#   cmake -S . -B build \
#         -DCMAKE_TOOLCHAIN_FILE=../../cmake/toolchain-ra8d2.cmake \
#         -DRA_USE_THREADX=ON -DRA_USE_FILEX=ON -DRA_USE_GUIX=ON
#   cmake --build build
#
# Pulled in by the top-level CMakeLists.txt the same way -- the demo
# silently disables itself when ThreadX, FileX, or GUIX is off so the
# default `cmake -B build` keeps building blink/.
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

    project(ereader
        LANGUAGES C CXX ASM
        VERSION ${RA_PROJECT_VERSION}
        DESCRIPTION "ePub reader skeleton for EK-RA8D2"
    )

    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    endif()

    set(CMAKE_C_STANDARD 23)
    set(CMAKE_C_STANDARD_REQUIRED ON)
    set(CMAKE_C_EXTENSIONS ON)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

    set(CMAKE_C_FLAGS_DEBUG          "-O0 -g3 -DDEBUG")
    set(CMAKE_ASM_FLAGS_DEBUG        "-g3")
    set(CMAKE_C_FLAGS_RELEASE        "-Os -g1 -DNDEBUG")
    set(CMAKE_ASM_FLAGS_RELEASE      "-g1")
    set(CMAKE_C_FLAGS_RELWITHDEBINFO "-Og -g3")
    set(CMAKE_ASM_FLAGS_RELWITHDEBINFO "-g3")

    option(RA_TRUSTZONE_ENABLE "Enable Cortex-M85 TrustZone (-mcmse + SAU)" OFF)
    option(RA_USE_THREADX      "Enable the vendored ThreadX RTOS"           ON)
    option(RA_USE_FILEX        "Enable the vendored FileX FAT file system"  ON)
    option(RA_USE_GUIX         "Enable the vendored GUIX UI framework"      ON)
    option(RA_USE_LEVELX       "Enable the vendored LevelX NOR wear leveler" OFF)

    include(${RA_REPO_ROOT}/cmake/threadx.cmake OPTIONAL RESULT_VARIABLE _tx_inc)
    include(${RA_REPO_ROOT}/cmake/filex.cmake)
    include(${RA_REPO_ROOT}/cmake/guix.cmake)
    if(RA_USE_LEVELX)
        include(${RA_REPO_ROOT}/cmake/levelx.cmake)
    endif()
else()
    get_filename_component(RA_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
endif()

# Skip the executable when any required component is off. Top-level
# `cmake -B build` walks examples/* by default and we do not want to
# drag the vendored trees into the bare-metal-only configuration.
if(NOT RA_USE_THREADX OR NOT RA_USE_FILEX OR NOT RA_USE_GUIX)
    message(STATUS "ereader: skipped (RA_USE_THREADX=${RA_USE_THREADX}, RA_USE_FILEX=${RA_USE_FILEX}, RA_USE_GUIX=${RA_USE_GUIX})")
    return()
endif()

include(${RA_REPO_ROOT}/cmake/ra_warnings.cmake)

#############################################################################
# Sources
#############################################################################

set(RA_APP_NAME ereader)

set(APP_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/power.c
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

# ra_epub is a domain library (Wave 14); always pull it in for ereader.
# It depends on libs/third_party/miniz (ZIP), libs/third_party/tinyxml2
# (XML), and libs/third_party/stb (glyphs + image decode).
file(GLOB_RECURSE LIB_RA_EPUB_C_SOURCES   CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_epub/src/*.c)
file(GLOB_RECURSE LIB_RA_EPUB_CPP_SOURCES CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_epub/src/*.cpp)
set(LIB_THIRD_PARTY_SOURCES
    ${RA_REPO_ROOT}/libs/third_party/miniz/miniz.c
    ${RA_REPO_ROOT}/libs/third_party/tinyxml2/tinyxml2.cpp
)

# ra_reflow is a sibling sweep deliverable (Wave 11 / Sweep 15). The
# header lives at ``libs/ra_reflow/inc/ra_reflow.h``; the engine
# requires a TTF font blob, an ``ra_gfx`` framebuffer binding and a
# proper viewport. Wiring that all up against the EK-RA8D2 panel is
# Phase 6.2 work; the skeleton only defines ``RA_HAS_REFLOW`` when
# the user explicitly opts in via ``-DRA_HAS_REFLOW=ON``.
#
# Sources are still pulled in unconditionally when present so the
# build link-tests the library, but the call sites in main.c stay
# gated on ``RA_HAS_REFLOW``.
option(RA_HAS_REFLOW "Wire ra_reflow into the ereader render path" OFF)
option(RA_REFLOW_USE_LITEHTML
       "Use the LiteHTML-backed v2 ra_reflow engine (default OFF: keeps v1)"
       OFF)
if(RA_REFLOW_USE_LITEHTML)
    set(LIB_RA_REFLOW_SOURCES ${RA_REPO_ROOT}/libs/ra_reflow/v2/ra_reflow_v2.cpp)
    set(LIB_RA_REFLOW_CPP_SOURCES "")
else()
    file(GLOB LIB_RA_REFLOW_SOURCES CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_reflow/src/*.c)
    file(GLOB LIB_RA_REFLOW_CPP_SOURCES CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_reflow/src/*.cpp)
endif()

# Pull in the FileX <-> SDHI and GUIX <-> GLCDC port shims via the
# global properties that port/{filex,guix}/CMakeLists.txt populate.
get_property(RA_FILEX_PORT_SOURCES GLOBAL PROPERTY RA_FILEX_PORT_SOURCES)
get_property(RA_GUIX_PORT_SOURCES  GLOBAL PROPERTY RA_GUIX_PORT_SOURCES)

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
    ${LIB_RA_EPUB_C_SOURCES}
    ${LIB_RA_EPUB_CPP_SOURCES}
    ${LIB_RA_REFLOW_SOURCES}
    ${LIB_RA_REFLOW_CPP_SOURCES}
    ${LIB_THIRD_PARTY_SOURCES}
    ${RA_FILEX_PORT_SOURCES}
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
    ${RA_REPO_ROOT}/libs/ra_epub/inc
    ${RA_REPO_ROOT}/libs/ra_reflow/inc
    ${RA_REPO_ROOT}/libs/third_party/miniz
    ${RA_REPO_ROOT}/libs/third_party/tinyxml2
    ${RA_REPO_ROOT}/libs/third_party/stb
    ${RA_REPO_ROOT}/port/guix
)

target_compile_definitions(${RA_APP_NAME}.elf PRIVATE
    RA_USE_THREADX
    RA_USE_FILEX
    RA_USE_GUIX
    RA_GFX_USE_DRW=1
)
if(RA_HAS_REFLOW)
    target_compile_definitions(${RA_APP_NAME}.elf PRIVATE RA_HAS_REFLOW)
endif()

# miniz config -- the bare-metal target does not have fopen / utime
# / etc. Disable the stdio + writer paths so only the in-memory ZIP
# reader is compiled. The vendored miniz also has stack frames
# beyond the project's 4400-byte ceiling (decompress / read-central-
# dir paths each hold a tinfl_decompressor on stack); suppress
# stack-usage / type-limits for this file only.
set_source_files_properties(
    ${RA_REPO_ROOT}/libs/third_party/miniz/miniz.c
    PROPERTIES COMPILE_FLAGS
        "-DMINIZ_NO_STDIO -DMINIZ_NO_TIME -DMINIZ_NO_ARCHIVE_WRITING_APIS -Wno-error=type-limits -Wno-error=stack-usage="
)

# X-Ware (and ThreadX, transitively) come in as INTERFACE libraries.
target_link_libraries(${RA_APP_NAME}.elf PRIVATE
    filex
    filex_port_ra_sdhi
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
