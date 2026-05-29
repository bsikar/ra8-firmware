#
# cmake/ra_add_app.cmake -- shared per-app firmware build recipe.
#
# Each example app's CMakeLists.txt is reduced to a thin stub:
#
#     cmake_minimum_required(VERSION 3.20)
#     set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
#     while(NOT EXISTS "${_d}/cmake/ra_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
#         get_filename_component(_d "${_d}" DIRECTORY)
#     endwhile()
#     include("${_d}/cmake/ra_add_app.cmake")
#     ra_add_app(NAME blink STACK_BYTES 2200 DESCRIPTION "Bare-metal blink firmware")
#
# ra_add_app() builds <NAME>.elf/.hex/.bin from:
#   - main.c + vector_table.c        : always taken from the app dir
#   - system_init.c / secure_exception.c / trustzone_init.c :
#       the app's local copy if it exists (per-app override), else the
#       shared libs/ra_board_ek_ra8d2/boot/ copy
#   - linker_script.ld               : the app dir
#   - the ra_* libraries + src/secure_app
#
# Implemented as a macro so project()/set() land in the caller's
# directory scope (project() may not be called from a function).
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

# Captured at include time (file scope) so it points at cmake/, not the
# caller's dir -- inside a macro CMAKE_CURRENT_LIST_DIR is the caller's.
set(_RA_ADD_APP_DIR "${CMAKE_CURRENT_LIST_DIR}")

macro(ra_add_app)
    cmake_parse_arguments(_RA_APP "" "NAME;STACK_BYTES;DESCRIPTION" "" ${ARGN})

    if(NOT _RA_APP_NAME)
        message(FATAL_ERROR "ra_add_app(): NAME is required")
    endif()
    if(NOT _RA_APP_STACK_BYTES)
        set(_RA_APP_STACK_BYTES 2200)
    endif()
    if(NOT _RA_APP_DESCRIPTION)
        set(_RA_APP_DESCRIPTION "RA8D2 firmware: ${_RA_APP_NAME}")
    endif()

    # Repo root = parent of the dir holding this file (cmake/).
    get_filename_component(RA_REPO_ROOT "${_RA_ADD_APP_DIR}/.." ABSOLUTE)

    # ---- standalone vs embedded -------------------------------------------
    get_directory_property(_ra_has_parent PARENT_DIRECTORY)
    if(NOT _ra_has_parent)
        file(READ "${RA_REPO_ROOT}/VERSION" _ra_ver)
        string(STRIP "${_ra_ver}" _ra_ver)
        project(${_RA_APP_NAME} LANGUAGES C ASM VERSION ${_ra_ver}
                DESCRIPTION "${_RA_APP_DESCRIPTION}")
        if(NOT CMAKE_BUILD_TYPE)
            set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
        endif()
        set(CMAKE_C_STANDARD 23)
        set(CMAKE_C_STANDARD_REQUIRED ON)
        set(CMAKE_C_EXTENSIONS ON)
        set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
        set(CMAKE_C_FLAGS_DEBUG            "-O0 -g3 -DDEBUG")
        set(CMAKE_ASM_FLAGS_DEBUG          "-g3")
        set(CMAKE_C_FLAGS_RELEASE          "-Os -g1 -DNDEBUG")
        set(CMAKE_ASM_FLAGS_RELEASE        "-g1")
        set(CMAKE_C_FLAGS_RELWITHDEBINFO   "-Og -g3")
        set(CMAKE_ASM_FLAGS_RELWITHDEBINFO "-g3")
        option(RA_TRUSTZONE_ENABLE "Enable Cortex-M85 TrustZone (-mcmse + SAU)" OFF)
    endif()

    include(${RA_REPO_ROOT}/cmake/ra_warnings.cmake)

    # ---- sources: per-app main + vector table, shared-or-local boot -------
    set(_ra_src
        ${CMAKE_CURRENT_SOURCE_DIR}/main.c
        ${CMAKE_CURRENT_SOURCE_DIR}/vector_table.c
    )
    foreach(_ra_boot system_init.c secure_exception.c trustzone_init.c)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_ra_boot}")
            list(APPEND _ra_src "${CMAKE_CURRENT_SOURCE_DIR}/${_ra_boot}")
        else()
            list(APPEND _ra_src "${RA_REPO_ROOT}/libs/ra_board_ek_ra8d2/boot/${_ra_boot}")
        endif()
    endforeach()

    file(GLOB_RECURSE _ra_lib_core    CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_core/src/*.c)
    file(GLOB_RECURSE _ra_lib_hal     CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_hal/src/*.c)
    file(GLOB_RECURSE _ra_lib_net_pal CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_net_pal/src/*.c)
    file(GLOB_RECURSE _ra_lib_usb_pal CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_usb_pal/src/*.c)
    file(GLOB_RECURSE _ra_lib_nsc     CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_nsc/src/*.c)
    file(GLOB_RECURSE _ra_lib_board   CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_board_ek_ra8d2/src/*.c)
    file(GLOB_RECURSE _ra_secure_app  CONFIGURE_DEPENDS ${RA_REPO_ROOT}/src/secure_app/*.c)

    set(_ra_linker ${CMAKE_CURRENT_SOURCE_DIR}/linker_script.ld)
    set(_ra_elf ${_RA_APP_NAME}.elf)

    add_executable(${_ra_elf}
        ${_ra_src}
        ${_ra_lib_core} ${_ra_lib_hal} ${_ra_lib_net_pal} ${_ra_lib_usb_pal}
        ${_ra_lib_nsc} ${_ra_lib_board} ${_ra_secure_app})

    ra_target_enable_project_warnings(${_ra_elf} STACK_USAGE_BYTES ${_RA_APP_STACK_BYTES})
    target_compile_options(${_ra_elf} PRIVATE -fshort-enums)

    if(RA_TRUSTZONE_ENABLE)
        target_compile_definitions(${_ra_elf} PRIVATE RA_TRUSTZONE_ENABLE)
        target_compile_options(${_ra_elf} PRIVATE -mcmse)
        target_link_options(${_ra_elf} PRIVATE -mcmse)
    endif()

    target_include_directories(${_ra_elf} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${RA_REPO_ROOT}/src
        ${RA_REPO_ROOT}/src/inc
        ${RA_REPO_ROOT}/src/secure_app
        ${RA_REPO_ROOT}/libs/ra_core/inc
        ${RA_REPO_ROOT}/libs/ra_hal/inc
        ${RA_REPO_ROOT}/libs/ra_net_pal/inc
        ${RA_REPO_ROOT}/libs/ra_usb_pal/inc
        ${RA_REPO_ROOT}/libs/ra_nsc/inc
        ${RA_REPO_ROOT}/libs/ra_board_ek_ra8d2/inc)

    target_link_options(${_ra_elf} PRIVATE -T${_ra_linker} -Wl,--Map=${_RA_APP_NAME}.map)
    set_target_properties(${_ra_elf} PROPERTIES LINK_DEPENDS ${_ra_linker})

    add_custom_command(TARGET ${_ra_elf} POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${_ra_elf}> ${_RA_APP_NAME}.hex
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${_ra_elf}> ${_RA_APP_NAME}.bin
        COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${_ra_elf}>
        COMMENT "Generating ${_RA_APP_NAME}.hex / .bin and showing size")
endmacro()
