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
#   EXTRA_SRCS <f>...   explicit extra .c files compiled into this app (paths
#                       relative to the app dir or absolute). Each file's parent
#                       directory is added to the include path so a co-located
#                       header is found. Use this to share a helper TU across
#                       sibling apps (e.g. examples/.../common/foo.c) without a
#                       full library.
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
    cmake_parse_arguments(_RA_APP "NO_NSC" "NAME;STACK_BYTES;DESCRIPTION" "USES;LIBS;SIM_LIBS;NSC_SRCS;EXTRA_SRCS" ${ARGN})

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
    # NOTE: the per-app stub already contains its own literal, guarded
    # project() call -- CMake requires the top-level listfile of a standalone
    # `cmake -S . -B build` to contain a direct project() command, and a call
    # made here from inside a macro does not satisfy that rule. This second
    # project() re-affirms it with the shared VERSION and the app description;
    # it is cheap (compiler detection is already cached) and warning-free. Do
    # not remove the stub's project() to "dedupe" -- the dev warning returns.
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

    # Explicit shared helper TUs (EXTRA_SRCS): compiled into this app, with each
    # file's parent directory added to the include path so a co-located header is
    # found. This lets sibling apps share one helper .c (e.g. a common/ dir) via
    # the LIBS-style mechanism without promoting it to a full library.
    set(_ra_extra_inc "")
    foreach(_ra_extra ${_RA_APP_EXTRA_SRCS})
        if(IS_ABSOLUTE "${_ra_extra}")
            set(_ra_extra_abs "${_ra_extra}")
        else()
            get_filename_component(_ra_extra_abs "${CMAKE_CURRENT_SOURCE_DIR}/${_ra_extra}" ABSOLUTE)
        endif()
        list(APPEND _ra_src "${_ra_extra_abs}")
        get_filename_component(_ra_extra_dir "${_ra_extra_abs}" DIRECTORY)
        list(APPEND _ra_extra_inc "${_ra_extra_dir}")
    endforeach()
    if(_ra_extra_inc)
        list(REMOVE_DUPLICATES _ra_extra_inc)
    endif()

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

    # ra_epub parses .epub (ZIP container + XML) through the vendored miniz (C)
    # and tinyxml2 (C++). Its first-party .c sources are globbed by the LIBS loop
    # above, but the C-callable XML shim is C++ (ra_epub_xml_shim.cpp) and the two
    # third_party TUs live outside libs/ra_epub/src. Wire them when an app pulls in
    # ra_epub, mirroring the ra_reflow/stb special-case above. tinyxml2's MemPoolT
    # is redirected to a static arena inside the shim, and miniz's malloc to the
    # ra_epub_miniz_alloc static pool (both NASA Rule 3 deviations).
    set(_ra_epub_cpp "")
    set(_ra_epub_vendor "")
    if("ra_epub" IN_LIST _RA_APP_LIBS)
        file(GLOB_RECURSE _ra_epub_cpp CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_epub/src/*.cpp)
        set(_ra_epub_vendor
            ${RA_REPO_ROOT}/libs/third_party/miniz/miniz.c
            ${RA_REPO_ROOT}/libs/third_party/tinyxml2/tinyxml2.cpp)
        list(APPEND _ra_lib_extra ${_ra_epub_cpp} ${_ra_epub_vendor})
        list(APPEND _ra_lib_inc
            ${RA_REPO_ROOT}/libs/third_party/miniz
            ${RA_REPO_ROOT}/libs/third_party/tinyxml2
            ${RA_REPO_ROOT}/libs/ra_epub/src)
    endif()

    # A bare "miniz" in LIBS pulls in just the vendored DEFLATE core, for apps
    # that inflate compressed blobs directly (e.g. ra_book RBKZ containers via the
    # heap-free tinfl_decompress) without ra_epub's full ZIP + XML stack. Skipped
    # when ra_epub is present, which already compiles miniz.c above.
    set(_ra_miniz_vendor "")
    if(("miniz" IN_LIST _RA_APP_LIBS) AND (NOT "ra_epub" IN_LIST _RA_APP_LIBS))
        set(_ra_miniz_vendor ${RA_REPO_ROOT}/libs/third_party/miniz/miniz.c)
        list(APPEND _ra_lib_extra ${_ra_miniz_vendor})
        list(APPEND _ra_lib_inc ${RA_REPO_ROOT}/libs/third_party/miniz)
    endif()

    # ra_io_compress.c and ra_io_vfs_compress.c (globbed in by a bare "ra_io" in
    # LIBS) drive the vendored miniz DEFLATE core, so they only link when the app
    # also supplies that core -- a bare "miniz" or the full "ra_epub" stack. The
    # VFS compression layer (ra_io_vfs_compress.c) is the transparent
    # compress-on-write / decompress-on-read fabric seam and pulls miniz through
    # ra_io_compress.h, so it shares the same opt-in gate. For plain ra_io
    # consumers (the common case) drop both from the glob so they need no
    # compressor and no miniz include path. The headers (ra_io_compress.h and
    # ra_io_vfs_compress.h) are likewise opt-in, kept out of the ra_io.h umbrella,
    # so an app that never compresses pays nothing.
    if(NOT (("miniz" IN_LIST _RA_APP_LIBS) OR ("ra_epub" IN_LIST _RA_APP_LIBS)))
        list(FILTER _ra_lib_extra EXCLUDE REGEX "ra_io/src/ra_io_compress\\.c$")
        list(FILTER _ra_lib_extra EXCLUDE REGEX "ra_io/src/ra_io_vfs_compress\\.c$")
    endif()

    # ra_io_blockdev_vsource.c (globbed in by a bare "ra_io" in LIBS) is the
    # sanctioned Ring-4 -> Ring-2 bridge that exposes a block device as an
    # ra_vsource read callback for the issue #147 page cache. It includes
    # ra_vsource.h from ra_mem, so it only links -- and only adds ra_mem/inc to
    # the include path -- when the app also declares "ra_mem" in LIBS. Mirrors the
    # ra_io_compress opt-in gate above: a plain ra_io consumer that never wires a
    # block device into the page cache pays nothing and needs no ra_mem on its
    # include path. The header (ra_io_blockdev_vsource.h) is likewise opt-in, kept
    # out of the ra_io.h umbrella.
    if("ra_mem" IN_LIST _RA_APP_LIBS)
        list(APPEND _ra_lib_inc ${RA_REPO_ROOT}/libs/ra_mem/inc)
    else()
        list(FILTER _ra_lib_extra EXCLUDE REGEX "ra_io/src/ra_io_blockdev_vsource\\.c$")
    endif()

    # The vendored SOUP decoders (miniz DEFLATE, stb image/truetype) type-pun
    # through byte buffers, which violates C strict-aliasing. GCC's aliasing
    # optimizations at -Og/-O2 then miscompile them: arm-none-eabi-gcc 13.3
    # corrupts miniz's inflate so EPUB/RBKZ extraction fails (ra_epub_open ->
    # "FAIL open") on the official toolchain, while older toolchains happen not
    # to trip it. Build just these third_party TUs with -fno-strict-aliasing --
    # the upstream-sanctioned flag for this code -- so the -Og default is
    # correct on every toolchain. First-party sources stay strict-aliasing clean.
    set(_ra_soup_tu ${_ra_epub_vendor} ${_ra_miniz_vendor}
                    ${_ra_stb_impl} ${_ra_stb_img_impl})
    if(_ra_soup_tu)
        set_source_files_properties(${_ra_soup_tu}
            PROPERTIES COMPILE_OPTIONS -fno-strict-aliasing)
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

    # The vendored miniz.c / tinyxml2.cpp cannot satisfy the first-party -Werror
    # set; disable warnings for those two TUs only (the first-party ra_epub .c /
    # .cpp shim still go through the full warning set). MINIZ_NO_STDIO/NO_TIME
    # must be set for EVERY TU that includes miniz.h (the vendored miniz.c *and*
    # ra_epub's TUs) so the header ABI matches -- miniz.c references utime()/
    # fopen() (absent on bare-metal newlib) only under the default config, and a
    # mismatched config between miniz.c and ra_epub corrupts the reader. ra_epub
    # uses only the in-memory reader, so the file path is never needed.
    if(_ra_epub_vendor)
        set_property(SOURCE ${_ra_epub_vendor} APPEND PROPERTY COMPILE_OPTIONS -w)
        target_compile_definitions(${_ra_elf} PRIVATE MINIZ_NO_STDIO MINIZ_NO_TIME)
    endif()

    # miniz-only apps: same vendored-TU warning suppression + config defines, so
    # miniz.c and every first-party TU that includes miniz.h share one ABI.
    # NO_ARCHIVE_APIS drops the malloc-using ZIP reader the firmware never calls,
    # leaving the heap-free tinfl DEFLATE core (driven from a static decompressor).
    if(_ra_miniz_vendor)
        set_property(SOURCE ${_ra_miniz_vendor} APPEND PROPERTY COMPILE_OPTIONS -w)
        target_compile_definitions(${_ra_elf} PRIVATE
            MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_ARCHIVE_APIS)
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
        ${_ra_lib_inc}
        ${_ra_extra_inc})

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
