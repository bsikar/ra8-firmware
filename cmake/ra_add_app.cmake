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
# Options:
#   NAME <n>            (required) app + elf base name
#   STACK_BYTES <n>     per-function stack-frame budget (default 2200)
#   DESCRIPTION <s>     project() description for standalone builds
#   NO_NSC              exclude the ra_nsc sources (secure-only dual-core apps)
#   USES <m>...         vendored middleware to enable + link. Each <m> maps to
#                       cmake/<m>.cmake (interface lib target <m>) and, when a
#                       global property RA_<M>_PORT_SOURCES exists, that
#                       property's bridge sources. Recognised: threadx usbx
#                       netxduo filex levelx nimble mbedtls. In standalone
#                       builds RA_USE_<M> defaults ON; in the aggregate build it
#                       stays as the top-level set it, and an app whose
#                       middleware is OFF skips itself.
#   LIBS <l>...         extra first-party libraries under libs/<l> to glob +
#                       add to the include path.
#   SIM_LIBS <l>...     like LIBS, but the library's sources are compiled with
#                       RA_SIMULATOR_MODE defined.
#
# Implemented as a macro so project()/set()/return() land in the caller's
# directory scope (project() may not be called from a function, and the
# middleware skip-guard must return from the app's own CMakeLists).
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

# Captured at include time (file scope) so it points at cmake/, not the
# caller's dir -- inside a macro CMAKE_CURRENT_LIST_DIR is the caller's.
set(_RA_ADD_APP_DIR "${CMAKE_CURRENT_LIST_DIR}")

macro(ra_add_app)
    cmake_parse_arguments(_RA_APP "NO_NSC" "NAME;STACK_BYTES;DESCRIPTION" "USES;LIBS;SIM_LIBS;NSC_SRCS" ${ARGN})

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

    # ---- vendored middleware (USES) ---------------------------------------
    # For each requested middleware: default RA_USE_<M> ON in standalone
    # builds, skip the whole app if it is OFF, then pull in cmake/<m>.cmake
    # (which defines the interface library target <m>).
    foreach(_ra_use ${_RA_APP_USES})
        string(TOUPPER "${_ra_use}" _ra_use_up)
        if(NOT _ra_has_parent)
            option(RA_USE_${_ra_use_up} "Enable the vendored ${_ra_use}" ON)
        endif()
        if(NOT RA_USE_${_ra_use_up})
            message(STATUS "${_RA_APP_NAME}: skipped (RA_USE_${_ra_use_up} is OFF)")
            return()
        endif()
        if(NOT TARGET ${_ra_use})
            include(${RA_REPO_ROOT}/cmake/${_ra_use}.cmake OPTIONAL)
        endif()
    endforeach()

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

    # Optional app-local helper translation units: every .c under the app's
    # src/ subdirectory is compiled into this app, so a large single-file app
    # can be split into focused TUs (their headers sit alongside; src/ is added
    # to the include path below). Opt-in -- apps without a src/ dir are
    # unaffected, and app-root .c files (e.g. a cpu1_main.c built separately)
    # are never swept in.
    file(GLOB _ra_app_local CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
    list(APPEND _ra_src ${_ra_app_local})

    file(GLOB_RECURSE _ra_lib_core    CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_core/src/*.c)
    file(GLOB_RECURSE _ra_lib_hal     CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_hal/src/*.c)
    file(GLOB_RECURSE _ra_lib_net_pal CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_net_pal/src/*.c)
    file(GLOB_RECURSE _ra_lib_usb_pal CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_usb_pal/src/*.c)
    file(GLOB_RECURSE _ra_lib_board   CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_board_ek_ra8d2/src/*.c)
    file(GLOB_RECURSE _ra_secure_app  CONFIGURE_DEPENDS ${RA_REPO_ROOT}/src/secure_app/*.c)
    if(_RA_APP_NO_NSC)
        set(_ra_lib_nsc "")
    elseif(_RA_APP_NSC_SRCS)
        # Compile only the named ra_nsc sources (e.g. just ra_nsc_cgc.c) instead
        # of globbing all of libs/ra_nsc/src -- lets an app pull the CGC veneers
        # without dragging in ra_nsc_comms/ra_nsc_eth, which don't compile under
        # -mcmse (#54).
        set(_ra_lib_nsc "")
        foreach(_ra_nsc_src ${_RA_APP_NSC_SRCS})
            list(APPEND _ra_lib_nsc ${RA_REPO_ROOT}/libs/ra_nsc/src/${_ra_nsc_src})
        endforeach()
    else()
        file(GLOB_RECURSE _ra_lib_nsc CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_nsc/src/*.c)
    endif()

    # Extra first-party libraries (plain + simulator-mode).
    set(_ra_lib_extra "")
    set(_ra_lib_extra_sim "")
    set(_ra_lib_inc "")
    foreach(_ra_lib ${_RA_APP_LIBS})
        file(GLOB_RECURSE _ra_lib_one CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/${_ra_lib}/src/*.c)
        list(APPEND _ra_lib_extra ${_ra_lib_one})
        list(APPEND _ra_lib_inc ${RA_REPO_ROOT}/libs/${_ra_lib}/inc)
    endforeach()
    foreach(_ra_lib ${_RA_APP_SIM_LIBS})
        file(GLOB_RECURSE _ra_lib_one CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/${_ra_lib}/src/*.c)
        list(APPEND _ra_lib_extra_sim ${_ra_lib_one})
        list(APPEND _ra_lib_inc ${RA_REPO_ROOT}/libs/${_ra_lib}/inc)
    endforeach()

    # ra_reflow rasterises glyphs through the vendored stb_truetype. Its
    # implementation TU lives under third_party (not libs/ra_reflow/src), and
    # STBTT_malloc/free must be redirected to the no-heap bump arena in
    # ra_stbtt_alloc.c. Wire that automatically when an app pulls in ra_reflow,
    # mirroring tests/CMakeLists.txt so app + host-test builds stay in step.
    set(_ra_stb_impl "")
    set(_ra_stb_img_impl "")
    if("ra_reflow" IN_LIST _RA_APP_LIBS)
        set(_ra_stb_impl ${RA_REPO_ROOT}/libs/third_party/stb/stb_truetype_impl.c)
        # ra_reflow also decodes raster <img> / cover art through the vendored
        # stb_image, whose allocator is redirected to the heap-free bump arena in
        # ra_img_arena.c. That single-TU build (stb_image_impl.c) is self-contained
        # (the STBI_* macros are defined inside it), so it needs no -include here.
        set(_ra_stb_img_impl ${RA_REPO_ROOT}/libs/third_party/stb/stb_image_impl.c)
        list(APPEND _ra_lib_extra ${_ra_stb_impl} ${_ra_stb_img_impl})
        list(APPEND _ra_lib_inc
            ${RA_REPO_ROOT}/libs/third_party/stb
            ${RA_REPO_ROOT}/libs/ra_reflow/src)
    endif()

    set(_ra_linker ${CMAKE_CURRENT_SOURCE_DIR}/linker_script.ld)
    set(_ra_elf ${_RA_APP_NAME}.elf)

    add_executable(${_ra_elf}
        ${_ra_src}
        ${_ra_lib_core} ${_ra_lib_hal} ${_ra_lib_net_pal} ${_ra_lib_usb_pal}
        ${_ra_lib_nsc} ${_ra_lib_board} ${_ra_secure_app}
        ${_ra_lib_extra} ${_ra_lib_extra_sim})

    if(_ra_lib_extra_sim)
        set_source_files_properties(${_ra_lib_extra_sim}
            PROPERTIES COMPILE_DEFINITIONS "RA_SIMULATOR_MODE")
    endif()

    # Route stb_truetype's allocations through the heap-free arena (see above).
    # The vendored header cannot satisfy the first-party -Werror set, so warnings
    # are disabled for this single third_party TU (-w wins over the project flags).
    if(_ra_stb_impl)
        set_property(SOURCE ${_ra_stb_impl} APPEND PROPERTY COMPILE_OPTIONS
            -w
            -include "${RA_REPO_ROOT}/libs/ra_reflow/inc/ra_stbtt_alloc.h"
            "-DSTBTT_malloc(x,u)=ra_stbtt_malloc(x)"
            "-DSTBTT_free(x,u)=ra_stbtt_free(x)")
        # stb_truetype pulls in sqrt/floor/ceil from the math library.
        target_link_libraries(${_ra_elf} PRIVATE m)
    endif()

    # Same treatment for the stb_image single-TU build: a vendored TU that
    # cannot satisfy the first-party -Werror set, so warnings are disabled.
    # The STBI_* allocator macros are defined inside stb_image_impl.c itself.
    if(_ra_stb_img_impl)
        set_property(SOURCE ${_ra_stb_img_impl} APPEND PROPERTY COMPILE_OPTIONS -w)
    endif()

    ra_target_enable_project_warnings(${_ra_elf} STACK_USAGE_BYTES ${_RA_APP_STACK_BYTES})
    target_compile_options(${_ra_elf} PRIVATE -fshort-enums)

    # Vendored RTOS / middleware headers trip several strict-warning gates we
    # apply to first-party code (CHAR* params, redundant decls, casts,
    # redefined macros). Relax the gate for apps that pull them in; the rest
    # of the codebase still gets the full -Werror set.
    if(_RA_APP_USES)
        target_compile_options(${_ra_elf} PRIVATE
            -Wno-error=discarded-qualifiers
            -Wno-error=cast-qual
            -Wno-error=cast-align
            -Wno-error=redundant-decls
            -Wno-error=missing-prototypes
            -Wno-error=builtin-macro-redefined
            -Wno-error)
    endif()

    if(RA_TRUSTZONE_ENABLE)
        target_compile_definitions(${_ra_elf} PRIVATE RA_TRUSTZONE_ENABLE)
        target_compile_options(${_ra_elf} PRIVATE -mcmse)
        target_link_options(${_ra_elf} PRIVATE -mcmse)
    endif()

    target_include_directories(${_ra_elf} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${RA_REPO_ROOT}/src
        ${RA_REPO_ROOT}/src/inc
        ${RA_REPO_ROOT}/src/secure_app
        ${RA_REPO_ROOT}/libs/ra_core/inc
        ${RA_REPO_ROOT}/libs/ra_hal/inc
        ${RA_REPO_ROOT}/libs/ra_net_pal/inc
        ${RA_REPO_ROOT}/libs/ra_usb_pal/inc
        ${RA_REPO_ROOT}/libs/ra_nsc/inc
        ${RA_REPO_ROOT}/libs/ra_board_ek_ra8d2/inc
        ${_ra_lib_inc})

    # Each middleware <m> ships a board-port interface library <m>_port_<bus>
    # that carries the RA-specific bridge headers; link it alongside the core.
    set(_ra_port_lib_threadx "")
    set(_ra_port_lib_usbx    usbx_port_ra_usb)
    set(_ra_port_lib_netxduo netxduo_port_ra_eth)
    set(_ra_port_lib_mbedtls mbedtls_port_ra_rsip)
    set(_ra_port_lib_filex   filex_port_ra_sdhi)
    set(_ra_port_lib_levelx  levelx_port_ra_xspi)
    set(_ra_port_lib_nimble  nimble_port_threadx)

    # Link the middleware interface libraries + pull in their bridge sources.
    foreach(_ra_use ${_RA_APP_USES})
        string(TOUPPER "${_ra_use}" _ra_use_up)
        if(TARGET ${_ra_use})
            target_link_libraries(${_ra_elf} PRIVATE ${_ra_use})
        endif()
        if(_ra_port_lib_${_ra_use} AND TARGET ${_ra_port_lib_${_ra_use}})
            target_link_libraries(${_ra_elf} PRIVATE ${_ra_port_lib_${_ra_use}})
        endif()
        get_property(_ra_port GLOBAL PROPERTY RA_${_ra_use_up}_PORT_SOURCES)
        if(_ra_port)
            target_sources(${_ra_elf} PRIVATE ${_ra_port})
        endif()
    endforeach()

    target_link_options(${_ra_elf} PRIVATE -T${_ra_linker} -Wl,--Map=${_RA_APP_NAME}.map)
    set_target_properties(${_ra_elf} PROPERTIES LINK_DEPENDS ${_ra_linker})

    add_custom_command(TARGET ${_ra_elf} POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${_ra_elf}> ${_RA_APP_NAME}.hex
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${_ra_elf}> ${_RA_APP_NAME}.bin
        COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${_ra_elf}>
        COMMENT "Generating ${_RA_APP_NAME}.hex / .bin and showing size")
endmacro()
