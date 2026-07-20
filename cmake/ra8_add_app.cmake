#
# cmake/ra8_add_app.cmake -- shared per-app firmware build recipe.
#
# Each example app's CMakeLists.txt is reduced to a thin stub:
#
#     cmake_minimum_required(VERSION 3.20)
#     set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
#     while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
#         get_filename_component(_d "${_d}" DIRECTORY)
#     endwhile()
#     include("${_d}/cmake/ra8_add_app.cmake")
#     ra8_add_app(NAME blink STACK_BYTES 2200 DESCRIPTION "Bare-metal blink firmware")
#
# ra8_add_app() builds <NAME>.elf/.hex/.bin from:
#   - main.c                         : always taken from the app dir
#   - vector_table.c / system_init.c / secure_exception.c / nmi_exception.c /
#     trustzone_init.c :
#       the app's local copy if it exists (per-app override), else the
#       selected board's libs/ra8_board_<BOARD>/boot/ copy (BOARD defaults to
#       ek_ra8d2 -- see the option below). The shared vector_table.c is
#       the plurality variant (weak-alias handlers + ra8_lpm_safe_boot() early
#       hook); apps with a divergent table (ra8_isr_dispatch dispatcher, no LPM
#       hook, dual-core / TrustZone reset) keep their own copy and override it.
#   - linker_script.ld               : the app's local copy if it exists
#       (divergent maps: dual-core, TrustZone, bootloader banks), else the
#       selected board's canonical single-core map
#       libs/ra8_board_<BOARD>/ld/linker_script.ld
#   - the ra8_* libraries + src/secure_app
#
# Options:
#   NAME <n>            (required) app + elf base name
#   STACK_BYTES <n>     per-function stack-frame budget (default 2200)
#   DESCRIPTION <s>     project() description for standalone builds
#   BOARD <b>           board-support layer under libs/ra8_board_<b> (default
#                       ek_ra8d2). Selects which board layer supplies the fallback
#                       boot files, the board src glob, the fallback linker script,
#                       and the board include path. Left unset every existing app
#                       resolves to libs/ra8_board_ek_ra8d2 -- the exact hardcoded
#                       paths this recipe used before the parameter existed, so the
#                       default build is byte-for-behaviour unchanged. The RA8P1
#                       foundation apps pass BOARD ra8p1 to build against the
#                       libs/ra8_board_ra8p1 layer (issue #226).
#   NO_NSC              exclude the ra8_nsc sources (secure-only dual-core apps)
#   USES <m>...         vendored middleware to enable + link. Each <m> maps to
#                       cmake/<m>.cmake (interface lib target <m>) and, when a
#                       global property RA8_<M>_PORT_SOURCES exists, that
#                       property's bridge sources. Recognised: threadx usbx
#                       netxduo filex levelx nimble mbedtls. In standalone
#                       builds RA8_USE_<M> defaults ON; in the aggregate build it
#                       stays as the top-level set it, and an app whose
#                       middleware is OFF skips itself.
#   LIBS <l>...         extra first-party libraries under libs/<l> to glob +
#                       add to the include path.
#   SIM_LIBS <l>...     like LIBS, but the library's sources are compiled with
#                       RA8_SIMULATOR_MODE defined.
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
set(_RA8_ADD_APP_DIR "${CMAKE_CURRENT_LIST_DIR}")

macro(ra8_add_app)
    cmake_parse_arguments(_RA8_APP "NO_NSC" "NAME;STACK_BYTES;DESCRIPTION;BOARD" "USES;LIBS;SIM_LIBS;NSC_SRCS;EXTRA_SRCS" ${ARGN})

    if(NOT _RA8_APP_NAME)
        message(FATAL_ERROR "ra8_add_app(): NAME is required")
    endif()
    if(NOT _RA8_APP_STACK_BYTES)
        set(_RA8_APP_STACK_BYTES 2200)
    endif()
    if(NOT _RA8_APP_DESCRIPTION)
        set(_RA8_APP_DESCRIPTION "RA8D2 firmware: ${_RA8_APP_NAME}")
    endif()
    # Board-support layer selector (issue #226). Unset -> ek_ra8d2, so every
    # existing app resolves to the exact libs/ra8_board_ek_ra8d2 paths this recipe
    # hardcoded before the parameter existed and rebuilds byte-identically.
    if(NOT _RA8_APP_BOARD)
        set(_RA8_APP_BOARD "ek_ra8d2")
    endif()

    # Repo root = parent of the dir holding this file (cmake/).
    get_filename_component(RA8_REPO_ROOT "${_RA8_ADD_APP_DIR}/.." ABSOLUTE)

    # Absolute path to the selected board layer. All four board references below
    # (fallback boot files, board src glob, fallback linker script, board include
    # path) derive from this one variable so the board is swapped in one place.
    set(_ra8_board_dir "${RA8_REPO_ROOT}/libs/ra8_board_${_RA8_APP_BOARD}")
    if(NOT EXISTS "${_ra8_board_dir}")
        message(FATAL_ERROR
            "ra8_add_app(): BOARD '${_RA8_APP_BOARD}' has no layer at ${_ra8_board_dir}")
    endif()

    # ---- standalone vs embedded -------------------------------------------
    # NOTE: the per-app stub already contains its own literal, guarded
    # project() call -- CMake requires the top-level listfile of a standalone
    # `cmake -S . -B build` to contain a direct project() command, and a call
    # made here from inside a macro does not satisfy that rule. This second
    # project() re-affirms it with the shared VERSION and the app description;
    # it is cheap (compiler detection is already cached) and warning-free. Do
    # not remove the stub's project() to "dedupe" -- the dev warning returns.
    get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
    if(NOT _ra8_has_parent)
        file(READ "${RA8_REPO_ROOT}/VERSION" _ra8_ver)
        string(STRIP "${_ra8_ver}" _ra8_ver)
        project(${_RA8_APP_NAME} LANGUAGES C ASM VERSION ${_ra8_ver}
                DESCRIPTION "${_RA8_APP_DESCRIPTION}")
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
        option(RA8_TRUSTZONE_ENABLE "Enable Cortex-M85 TrustZone (-mcmse + SAU)" OFF)
    endif()

    # ---- vendored middleware (USES) ---------------------------------------
    # For each requested middleware: default RA8_USE_<M> ON in standalone
    # builds, skip the whole app if it is OFF, then pull in cmake/<m>.cmake
    # (which defines the interface library target <m>).
    foreach(_ra8_use ${_RA8_APP_USES})
        string(TOUPPER "${_ra8_use}" _ra8_use_up)
        if(NOT _ra8_has_parent)
            option(RA8_USE_${_ra8_use_up} "Enable the vendored ${_ra8_use}" ON)
        endif()
        if(NOT RA8_USE_${_ra8_use_up})
            message(STATUS "${_RA8_APP_NAME}: skipped (RA8_USE_${_ra8_use_up} is OFF)")
            return()
        endif()
        if(NOT TARGET ${_ra8_use})
            include(${RA8_REPO_ROOT}/cmake/${_ra8_use}.cmake OPTIONAL)
        endif()
    endforeach()

    # ---- insecure placeholder-crypto opt-in (issue #180) ------------------
    # Several secure-side TUs (src/secure_app/{secure_trng,key_import,key_vault}.c
    # and libs/ra8_hal/src/ra8_rsip_key_injection.c) ship an INSECURE placeholder
    # body (a deterministic PRNG "TRNG", a forgeable key-import MAC, a plain-SRAM
    # key vault, a non-cryptographic RSIP key-wrap) that is only safe under host
    # simulation or an explicitly-declared dev/eval image. Each such body is
    # wrapped in `#if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_SIMULATOR_MODE)`
    # and its `#else` fails closed (every entry point returns a hard error). This
    # option is OFF by default so a release/HIL firmware image that forgets to
    # swap in a real crypto backend fails closed instead of silently shipping the
    # stub. A dev/eval firmware image opts in with -DRA8_INSECURE_STUB_CRYPTO=ON.
    option(RA8_INSECURE_STUB_CRYPTO "compile insecure placeholder crypto (host/sim only)" OFF)

    include(${RA8_REPO_ROOT}/cmake/ra8_warnings.cmake)
    include(${RA8_REPO_ROOT}/cmake/ra8_shared_libs.cmake)

    # ---- sources: per-app main, shared-or-local boot ----------------------
    set(_ra8_src
        ${CMAKE_CURRENT_SOURCE_DIR}/main.c
    )
    foreach(_ra8_boot vector_table.c system_init.c secure_exception.c nmi_exception.c trustzone_init.c)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_ra8_boot}")
            list(APPEND _ra8_src "${CMAKE_CURRENT_SOURCE_DIR}/${_ra8_boot}")
        else()
            list(APPEND _ra8_src "${_ra8_board_dir}/boot/${_ra8_boot}")
        endif()
    endforeach()

    # Optional app-local helper translation units: every .c under the app's
    # src/ subdirectory is compiled into this app, so a large single-file app
    # can be split into focused TUs (their headers sit alongside; src/ is added
    # to the include path below). Opt-in -- apps without a src/ dir are
    # unaffected, and app-root .c files (e.g. a cpu1_main.c built separately)
    # are never swept in.
    file(GLOB _ra8_app_local CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
    list(APPEND _ra8_src ${_ra8_app_local})

    # Explicit shared helper TUs (EXTRA_SRCS): compiled into this app, with each
    # file's parent directory added to the include path so a co-located header is
    # found. This lets sibling apps share one helper .c (e.g. a common/ dir) via
    # the LIBS-style mechanism without promoting it to a full library.
    set(_ra8_extra_inc "")
    foreach(_ra8_extra ${_RA8_APP_EXTRA_SRCS})
        if(IS_ABSOLUTE "${_ra8_extra}")
            set(_ra8_extra_abs "${_ra8_extra}")
        else()
            get_filename_component(_ra8_extra_abs "${CMAKE_CURRENT_SOURCE_DIR}/${_ra8_extra}" ABSOLUTE)
        endif()
        list(APPEND _ra8_src "${_ra8_extra_abs}")
        get_filename_component(_ra8_extra_dir "${_ra8_extra_abs}" DIRECTORY)
        list(APPEND _ra8_extra_inc "${_ra8_extra_dir}")
    endforeach()
    if(_ra8_extra_inc)
        list(REMOVE_DUPLICATES _ra8_extra_inc)
    endif()

    file(GLOB_RECURSE _ra8_lib_core    CONFIGURE_DEPENDS ${RA8_REPO_ROOT}/libs/ra8_core/src/*.c)
    file(GLOB_RECURSE _ra8_lib_hal     CONFIGURE_DEPENDS ${RA8_REPO_ROOT}/libs/ra8_hal/src/*.c)
    file(GLOB_RECURSE _ra8_lib_net_pal CONFIGURE_DEPENDS ${RA8_REPO_ROOT}/libs/ra8_net_pal/src/*.c)
    file(GLOB_RECURSE _ra8_lib_usb_pal CONFIGURE_DEPENDS ${RA8_REPO_ROOT}/libs/ra8_usb_pal/src/*.c)
    file(GLOB_RECURSE _ra8_lib_board   CONFIGURE_DEPENDS ${_ra8_board_dir}/src/*.c)
    file(GLOB_RECURSE _ra8_secure_app  CONFIGURE_DEPENDS ${RA8_REPO_ROOT}/src/secure_app/*.c)
    if(_RA8_APP_NO_NSC)
        set(_ra8_lib_nsc "")
    elseif(_RA8_APP_NSC_SRCS)
        # Compile only the named ra8_nsc sources (e.g. just ra8_nsc_cgc.c) instead
        # of globbing all of libs/ra8_nsc/src -- lets an app pull the CGC veneers
        # without dragging in ra8_nsc_comms/ra8_nsc_eth, which don't compile under
        # -mcmse (#54).
        set(_ra8_lib_nsc "")
        foreach(_ra8_nsc_src ${_RA8_APP_NSC_SRCS})
            list(APPEND _ra8_lib_nsc ${RA8_REPO_ROOT}/libs/ra8_nsc/src/${_ra8_nsc_src})
        endforeach()
    else()
        file(GLOB_RECURSE _ra8_lib_nsc CONFIGURE_DEPENDS ${RA8_REPO_ROOT}/libs/ra8_nsc/src/*.c)
    endif()

    # Extra first-party libraries (plain + simulator-mode).
    set(_ra8_lib_extra "")
    set(_ra8_lib_extra_sim "")
    set(_ra8_lib_inc "")
    foreach(_ra8_lib ${_RA8_APP_LIBS})
        file(GLOB_RECURSE _ra8_lib_one CONFIGURE_DEPENDS ${RA8_REPO_ROOT}/libs/${_ra8_lib}/src/*.c)
        list(APPEND _ra8_lib_extra ${_ra8_lib_one})
        list(APPEND _ra8_lib_inc ${RA8_REPO_ROOT}/libs/${_ra8_lib}/inc)
    endforeach()
    foreach(_ra8_lib ${_RA8_APP_SIM_LIBS})
        file(GLOB_RECURSE _ra8_lib_one CONFIGURE_DEPENDS ${RA8_REPO_ROOT}/libs/${_ra8_lib}/src/*.c)
        list(APPEND _ra8_lib_extra_sim ${_ra8_lib_one})
        list(APPEND _ra8_lib_inc ${RA8_REPO_ROOT}/libs/${_ra8_lib}/inc)
    endforeach()

    # ra8_reflow rasterises glyphs through the vendored stb_truetype. Its
    # implementation TU lives under third_party (not libs/ra8_reflow/src), and
    # STBTT_malloc/free must be redirected to the no-heap bump arena in
    # ra8_stbtt_alloc.c. Wire that automatically when an app pulls in ra8_reflow,
    # mirroring tests/CMakeLists.txt so app + host-test builds stay in step.
    set(_ra8_stb_impl "")
    set(_ra8_stb_img_impl "")
    if("ra8_reflow" IN_LIST _RA8_APP_LIBS)
        set(_ra8_stb_impl ${RA8_REPO_ROOT}/libs/third_party/stb/stb_truetype_impl.c)
        # ra8_reflow also decodes raster <img> / cover art through the vendored
        # stb_image, whose allocator is redirected to the heap-free bump arena in
        # ra8_img_arena.c. That single-TU build (stb_image_impl.c) is self-contained
        # (the STBI_* macros are defined inside it), so it needs no -include here.
        set(_ra8_stb_img_impl ${RA8_REPO_ROOT}/libs/third_party/stb/stb_image_impl.c)
        list(APPEND _ra8_lib_extra ${_ra8_stb_impl} ${_ra8_stb_img_impl})
        list(APPEND _ra8_lib_inc
            ${RA8_REPO_ROOT}/libs/third_party/stb
            ${RA8_REPO_ROOT}/libs/ra8_reflow/src)
    endif()

    # ra8_reflow's glyph rasteriser (ra8_reflow_render.c, #164) caches glyph bitmaps
    # through the Layer-3 ra8_glyph_atlas, and ra8_book's paged accessor
    # (ra8_book_paged.c / ra8_book_xhtml.c, #163) reads books through the ra8_vmem
    # page cache -- both in libs/ra8_mem. ra8_mem depends only on ra8_core, so wire
    # it in automatically for ra8_reflow / ra8_book consumers (mirroring the stb
    # special-case above), unless the app already lists ra8_mem in LIBS -- in which
    # case the loop above already globbed it.
    if((("ra8_reflow" IN_LIST _RA8_APP_LIBS) OR ("ra8_book" IN_LIST _RA8_APP_LIBS)) AND
       (NOT "ra8_mem" IN_LIST _RA8_APP_LIBS))
        file(GLOB_RECURSE _ra8_lib_mem CONFIGURE_DEPENDS ${RA8_REPO_ROOT}/libs/ra8_mem/src/*.c)
        list(APPEND _ra8_lib_extra ${_ra8_lib_mem})
        list(APPEND _ra8_lib_inc ${RA8_REPO_ROOT}/libs/ra8_mem/inc)
    endif()

    # ra8_ftl is a wrapper backend: it implements the ra8_io block-device vtable
    # (ra8_io_blockdev_iface), whose concrete layout lives in ra8_io's private
    # src header ra8_io_blockdev_internal.h. Expose that path when an app pulls in
    # ra8_ftl so ra8_ftl.c compiles in an app build, mirroring tests/CMakeLists.txt
    # (which already adds libs/ra8_io/src). ra8_io itself must be listed in LIBS to
    # supply the block-device fabric sources ra8_ftl links against.
    if("ra8_ftl" IN_LIST _RA8_APP_LIBS)
        list(APPEND _ra8_lib_inc ${RA8_REPO_ROOT}/libs/ra8_io/src)
    endif()

    # ra8_epub parses .epub (ZIP container + XML) through the vendored miniz (C)
    # and tinyxml2 (C++). Its first-party .c sources are globbed by the LIBS loop
    # above, but the C-callable XML shim is C++ (ra8_epub_xml_shim.cpp) and the two
    # third_party TUs live outside libs/ra8_epub/src. Wire them when an app pulls in
    # ra8_epub, mirroring the ra8_reflow/stb special-case above. tinyxml2's MemPoolT
    # is redirected to a static arena inside the shim, and miniz's malloc to the
    # ra8_epub_miniz_alloc static pool (both NASA Rule 3 deviations).
    set(_ra8_epub_cpp "")
    set(_ra8_epub_vendor "")
    if("ra8_epub" IN_LIST _RA8_APP_LIBS)
        file(GLOB_RECURSE _ra8_epub_cpp CONFIGURE_DEPENDS ${RA8_REPO_ROOT}/libs/ra8_epub/src/*.cpp)
        set(_ra8_epub_vendor
            ${RA8_REPO_ROOT}/libs/third_party/miniz/miniz.c
            ${RA8_REPO_ROOT}/libs/third_party/tinyxml2/tinyxml2.cpp)
        list(APPEND _ra8_lib_extra ${_ra8_epub_cpp} ${_ra8_epub_vendor})
        list(APPEND _ra8_lib_inc
            ${RA8_REPO_ROOT}/libs/third_party/miniz
            ${RA8_REPO_ROOT}/libs/third_party/tinyxml2
            ${RA8_REPO_ROOT}/libs/ra8_epub/src)
    endif()

    # ra8_jof transcodes JPEG/PNG sources into JOF tile atlases (#231).
    # Its PNG decoder inflates through the vendored miniz and its tile codec
    # reuses ra8_io_compress, so wire the miniz + ra8_io includes when an app
    # pulls in ra8_jof. The miniz *implementation* TU comes from the
    # ra8_epub block above or the bare-miniz block below -- an app using
    # ra8_jof lists one of those alongside it. The single compress TU is
    # added directly when the app does not already link the whole ra8_io
    # fabric.
    if("ra8_jof" IN_LIST _RA8_APP_LIBS)
        list(APPEND _ra8_lib_inc
            ${RA8_REPO_ROOT}/libs/third_party/miniz
            ${RA8_REPO_ROOT}/libs/ra8_io/inc)
        if(NOT "ra8_io" IN_LIST _RA8_APP_LIBS)
            list(APPEND _ra8_lib_extra ${RA8_REPO_ROOT}/libs/ra8_io/src/ra8_io_compress.c)
        endif()
        # #290 normalize-on-import: the producer normalises WebP manifest images
        # to JOF too, so it calls the ra8_webp facade (the WebP arm lives in
        # ra8_jof_produce_webp.c: ra8_jof_priv_webp_transcode). Compile the
        # facade sources here when the app did
        # not already list ra8_webp explicitly (the LIBS loop globs them then).
        # The vendored libwebp decoder itself is wired by the shared block below.
        list(APPEND _ra8_lib_inc ${RA8_REPO_ROOT}/libs/ra8_webp/inc)
        if(NOT "ra8_webp" IN_LIST _RA8_APP_LIBS)
            file(GLOB_RECURSE _ra8_jof_webp_facade CONFIGURE_DEPENDS
                ${RA8_REPO_ROOT}/libs/ra8_webp/src/*.c)
            list(APPEND _ra8_lib_extra ${_ra8_jof_webp_facade})
        endif()
    endif()

    # ra8_webp decodes WebP (VP8 / VP8L) through the vendored libwebp decoder
    # (libs/third_party/libwebp). The four-part recipe -- which TUs, which
    # include root, -DRA8_WEBP_USE_ARENA, the SOUP warning flags -- lives in
    # cmake/ra8_webp_vendor.cmake and is NOT restated here: open-coding it is
    # what left the recipe unreachable from a standalone host tool, which is why
    # tools/ra8_fmt and tools/media_dl each faked ra8_jof_priv_webp_transcode()
    # rather than compile the decoder that was already in the tree.
    #
    # ra8_webp's own .c facade/arena are globbed by the LIBS loop above (or by
    # the ra8_jof block); only the vendored TUs + include root are wired
    # here. Wired whenever ra8_webp is requested directly OR pulled in
    # transitively by ra8_jof (#290), and only once so the two paths never
    # double-add the libwebp sources.
    set(_ra8_webp_vendor "")
    if(("ra8_webp" IN_LIST _RA8_APP_LIBS) OR ("ra8_jof" IN_LIST _RA8_APP_LIBS))
        include(${RA8_REPO_ROOT}/cmake/ra8_webp_vendor.cmake)
        ra8_webp_vendor_sources(_ra8_webp_vendor ${RA8_REPO_ROOT})
        list(APPEND _ra8_lib_extra ${_ra8_webp_vendor})
        list(APPEND _ra8_lib_inc ${RA8_REPO_ROOT}/libs/third_party/libwebp)
    endif()

    # ra8_unarch decodes wrapped / container archive streams (tar for .cbt,
    # gzip, XZ) under the unified decompression-limits policy. Its XZ leg
    # drives the vendored xz-embedded decoder (SOUP), whose allocator and mode
    # selection come from the first-party porting header
    # libs/ra8_unarch/inc/xz_config.h (zero-heap pool, XZ_PREALLOC only).
    # Wired whenever ra8_unarch is requested directly OR pulled in
    # transitively by ra8_comic (the CBT / wrapped-open backends), and only
    # once. The gzip leg reuses the miniz DEFLATE core supplied by the
    # ra8_epub block above or the bare-miniz block below.
    set(_ra8_xz_vendor "")
    if(("ra8_unarch" IN_LIST _RA8_APP_LIBS) OR ("ra8_comic" IN_LIST _RA8_APP_LIBS))
        set(_ra8_xz_vendor
            ${RA8_REPO_ROOT}/libs/third_party/xz_embedded/xz_crc32.c
            ${RA8_REPO_ROOT}/libs/third_party/xz_embedded/xz_crc64.c
            ${RA8_REPO_ROOT}/libs/third_party/xz_embedded/xz_dec_lzma2.c
            ${RA8_REPO_ROOT}/libs/third_party/xz_embedded/xz_dec_stream.c)
        list(APPEND _ra8_lib_extra ${_ra8_xz_vendor})
        list(APPEND _ra8_lib_inc
            ${RA8_REPO_ROOT}/libs/third_party/xz_embedded
            ${RA8_REPO_ROOT}/libs/ra8_unarch/inc
            ${RA8_REPO_ROOT}/libs/third_party/miniz)
        if(NOT "ra8_unarch" IN_LIST _RA8_APP_LIBS)
            file(GLOB_RECURSE _ra8_unarch_srcs CONFIGURE_DEPENDS
                ${RA8_REPO_ROOT}/libs/ra8_unarch/src/*.c)
            list(APPEND _ra8_lib_extra ${_ra8_unarch_srcs})
        endif()
    endif()

    # A bare "miniz" in LIBS pulls in just the vendored DEFLATE core, for apps
    # that inflate compressed blobs directly (e.g. ra8_book RBKC containers via the
    # heap-free tinfl_decompress) without ra8_epub's full ZIP + XML stack. Skipped
    # when ra8_epub is present, which already compiles miniz.c above.
    set(_ra8_miniz_vendor "")
    if(("miniz" IN_LIST _RA8_APP_LIBS) AND (NOT "ra8_epub" IN_LIST _RA8_APP_LIBS))
        set(_ra8_miniz_vendor ${RA8_REPO_ROOT}/libs/third_party/miniz/miniz.c)
        list(APPEND _ra8_lib_extra ${_ra8_miniz_vendor})
        list(APPEND _ra8_lib_inc ${RA8_REPO_ROOT}/libs/third_party/miniz)
    endif()

    # ra8_io_compress.c and ra8_io_vfs_compress.c (globbed in by a bare "ra8_io" in
    # LIBS) drive the vendored miniz DEFLATE core, so they only link when the app
    # also supplies that core -- a bare "miniz" or the full "ra8_epub" stack. The
    # VFS compression layer (ra8_io_vfs_compress.c) is the transparent
    # compress-on-write / decompress-on-read fabric seam and pulls miniz through
    # ra8_io_compress.h, so it shares the same opt-in gate. For plain ra8_io
    # consumers (the common case) drop both from the glob so they need no
    # compressor and no miniz include path. The headers (ra8_io_compress.h and
    # ra8_io_vfs_compress.h) are likewise opt-in, kept out of the ra8_io.h umbrella,
    # so an app that never compresses pays nothing.
    if(NOT (("miniz" IN_LIST _RA8_APP_LIBS) OR ("ra8_epub" IN_LIST _RA8_APP_LIBS)))
        list(FILTER _ra8_lib_extra EXCLUDE REGEX "ra8_io/src/ra8_io_compress\\.c$")
        list(FILTER _ra8_lib_extra EXCLUDE REGEX "ra8_io/src/ra8_io_vfs_compress\\.c$")
    endif()

    # ra8_io_blockdev_vsource.c (globbed in by a bare "ra8_io" in LIBS) is the
    # sanctioned Ring-4 -> Ring-2 bridge that exposes a block device as an
    # ra8_vsource read callback for the issue #147 page cache. It includes
    # ra8_vsource.h from ra8_mem, so it only links -- and only adds ra8_mem/inc to
    # the include path -- when the app also declares "ra8_mem" in LIBS. Mirrors the
    # ra8_io_compress opt-in gate above: a plain ra8_io consumer that never wires a
    # block device into the page cache pays nothing and needs no ra8_mem on its
    # include path. The header (ra8_io_blockdev_vsource.h) is likewise opt-in, kept
    # out of the ra8_io.h umbrella.
    if("ra8_mem" IN_LIST _RA8_APP_LIBS)
        list(APPEND _ra8_lib_inc ${RA8_REPO_ROOT}/libs/ra8_mem/inc)
    else()
        list(FILTER _ra8_lib_extra EXCLUDE REGEX "ra8_io/src/ra8_io_blockdev_vsource\\.c$")
    endif()

    # A bare "ra8_io_bus" in LIBS pulls in just the ra8_io SPI/I2C bus facades
    # (ra8_io_spi_bus*.c, ra8_io_i2c_bus*.c) plus the libs/ra8_io/inc include
    # path, for apps that bind a device driver's bus seam (ra8_spi_bus_ops_t /
    # ra8_i2c_bus_ops_t) without the rest of the ra8_io fabric. The bus facade
    # TUs depend only on ra8_hal drivers, which every app already compiles, so
    # -- unlike the full "ra8_io" -- this needs no ra8_fs / ra8_sdmmc_spi /
    # ra8_usb_pal companions. Mirrors the bare-"miniz" pseudo-lib above; the
    # LIBS loop's libs/ra8_io_bus/src glob is harmlessly empty. Skipped when
    # the full "ra8_io" is present, which already compiles these TUs.
    if(("ra8_io_bus" IN_LIST _RA8_APP_LIBS) AND (NOT "ra8_io" IN_LIST _RA8_APP_LIBS))
        file(GLOB _ra8_io_bus_srcs CONFIGURE_DEPENDS
            ${RA8_REPO_ROOT}/libs/ra8_io/src/ra8_io_spi_bus*.c
            ${RA8_REPO_ROOT}/libs/ra8_io/src/ra8_io_i2c_bus*.c)
        list(APPEND _ra8_lib_extra ${_ra8_io_bus_srcs})
        list(APPEND _ra8_lib_inc ${RA8_REPO_ROOT}/libs/ra8_io/inc)
    endif()

    # The vendored SOUP decoders (miniz DEFLATE, stb image/truetype) type-pun
    # through byte buffers, which violates C strict-aliasing. GCC's aliasing
    # optimizations at -Og/-O2 then miscompile them: arm-none-eabi-gcc 13.3
    # corrupts miniz's inflate so EPUB/RBKC extraction fails (ra8_epub_open ->
    # "FAIL open") on the official toolchain, while older toolchains happen not
    # to trip it. Build just these third_party TUs with -fno-strict-aliasing --
    # the upstream-sanctioned flag for this code -- so the -Og default is
    # correct on every toolchain. First-party sources stay strict-aliasing clean.
    set(_ra8_soup_tu ${_ra8_epub_vendor} ${_ra8_miniz_vendor}
                    ${_ra8_stb_impl} ${_ra8_stb_img_impl})
    if(_ra8_soup_tu)
        set_source_files_properties(${_ra8_soup_tu}
            PROPERTIES COMPILE_OPTIONS -fno-strict-aliasing)
    endif()

    # Narrow warning suppression for the vendored SOUP parsers (issue #179).
    # These TUs used to carry a blanket -w, which switched OFF the ENTIRE
    # -Wall/-Wextra/-Werror profile -- including -Warray-bounds,
    # -Wstringop-overflow/-overread, and -Wmaybe-uninitialized, the cheapest
    # memory-safety diagnostics -- on exactly the attacker-controlled EPUB / ZIP
    # / image / font decode surface. A parser bug there is Non-secure code
    # execution. Replace -w with the minimal set of -Wno-<class> that silences
    # ONLY the style/pedantic noise these third_party TUs actually emit, leaving
    # -Werror in force for every other class so a real out-of-bounds /
    # uninitialised-read in the SOUP still breaks the build. Every name below was
    # confirmed to fire on a SOUP TU compiled with the full project warning
    # profile under arm-none-eabi-gcc 14.3 at -Og and -O2; none is a memory-safety
    # class. Split by language: the C-only names are themselves a hard -Werror
    # when handed to g++ ("valid for C/ObjC but not for C++"), so the C++ TU
    # (tinyxml2.cpp) receives the language-portable set only.
    set(_ra8_soup_wno_common
        -Wno-cast-qual          # miniz / stb cast away const on byte buffers
        -Wno-cast-align         # stb_image casts to a wider alignment
        -Wno-double-promotion   # stb float -> double in its math hooks
        -Wno-unused-parameter   # stb / tinyxml2 unused formals
        -Wno-type-limits        # miniz range-limited comparisons
        -Wno-duplicated-branches   # stb_image identical if/else arms
        -Wno-missing-declarations  # stb_image extern helpers with no prior decl
        -Wno-conversion            # stb / miniz implicit narrowing throughout
        -Wno-sign-conversion       # stb / miniz signed<->unsigned index math
        -Wno-float-conversion      # stb_truetype double -> float literals
        # stb_image's GIF path (stbi__gif_load / stbi__load_gif_main) puts ~34 KiB
        # of frame buffers on the stack -- far over the per-app -Wstack-usage=N
        # budget, but an inherent property of the vendored decoder we cannot shrink
        # without editing SOUP. The blanket -w hid this too; suppress only the
        # warning here. -fstack-usage still emits the .su data, so the real
        # project-wide stack-bound proof (scripts/utils/stack_usage_check.py over
        # the ARM .su files) still sees these frames.
        -Wno-stack-usage)
    set(_ra8_soup_wno_c
        -Wno-bad-function-cast     # stb_truetype casts a call result
        -Wno-missing-prototypes)   # stb globals with no prior prototype

    # linker_script.ld: app dir if present, else the shared canonical single-core
    # map. 125 apps carried a byte-identical script; the fallback lets them build
    # from one source so a region-map change touches one file, not 125 (T3-01).
    # Apps with a divergent map (dual-core, TrustZone slots, bootloader banks)
    # keep their own linker_script.ld and override the default.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/linker_script.ld")
        set(_ra8_linker ${CMAKE_CURRENT_SOURCE_DIR}/linker_script.ld)
    else()
        set(_ra8_linker ${_ra8_board_dir}/ld/linker_script.ld)
    endif()
    set(_ra8_elf ${_RA8_APP_NAME}.elf)

    # ---- prebuilt universal-library fast path (CI cross-build) ------------
    # When RA8_SHARED_LIB_ARCHIVE points at a prebuilt libra8_shared_<board>.a
    # (scripts/build_all_examples.sh sets it for the "Cross-build all apps"
    # gate), link that archive instead of recompiling the ~180 universal
    # first-party sources (ra8_core / ra8_hal / ra8_net_pal / ra8_usb_pal /
    # board / secure_app) into THIS executable -- they are compiled once for
    # the whole cross-build. --whole-archive pulls every member object (so the
    # link is identical to compiling the sources in) and the toolchain's
    # --gc-sections then prunes the unused ones, yielding the same final ELF
    # sections. The archive is board-independent code + the ek_ra8d2 board, and
    # is built for the RA8D2 (Cortex-M85, fpv5-sp-d16, no device define) with
    # TrustZone OFF and the placeholder crypto OFF, so an app is eligible only
    # when its own config matches: any other config falls back to the normal
    # per-source compile so its objects stay correct and its gates (e.g. the
    # tz_nsc_cgc_usb SG-veneer offsets) stay meaningful. In particular the RA8P1
    # tier (cmake/toolchain-ra8p1.cmake) builds the SAME shared sources with a
    # DIFFERENT device (-DRA8_DEVICE_RA8P1 -> ra8_npu_loader.c and register
    # bases change) AND a different FP ABI (double-precision fpv5-d16), so its
    # objects are symbol- and ABI-incompatible with this RA8D2 archive -- it is
    # excluded by the device-define check below (those apps set no BOARD, so the
    # board check alone would wrongly admit them).
    set(_ra8_shared_archive "$ENV{RA8_SHARED_LIB_ARCHIVE}")
    set(_ra8_use_shared_archive FALSE)
    if(_ra8_shared_archive
       AND EXISTS "${_ra8_shared_archive}"
       AND _RA8_APP_BOARD STREQUAL "ek_ra8d2"
       AND NOT RA8_TRUSTZONE_ENABLE
       AND NOT RA8_INSECURE_STUB_CRYPTO
       AND NOT CMAKE_C_FLAGS MATCHES "RA8_DEVICE_RA8P1")
        set(_ra8_use_shared_archive TRUE)
        # Drop the universal set from this app's own compile; the archive
        # supplies those objects. NSC (per-app subsets / -mcmse) and the extra
        # per-app libraries + SOUP TUs are NOT in the archive and stay here.
        set(_ra8_lib_core "")
        set(_ra8_lib_hal "")
        set(_ra8_lib_net_pal "")
        set(_ra8_lib_usb_pal "")
        set(_ra8_lib_board "")
        set(_ra8_secure_app "")
        # ~150 apps also name a universal-set library (usually
        # ra8_board_ek_ra8d2, some ra8_core / ra8_hal / ra8_usb_pal /
        # ra8_net_pal) explicitly in LIBS, which globbed its sources into
        # _ra8_lib_extra above -- a SECOND copy that would multiply-define
        # against the archive. Filter those exact universal source trees back
        # out of the extra list (their objects come from the archive; the
        # include paths the LIBS loop added stay). SIM_LIBS is never a
        # universal library, so _ra8_lib_extra_sim needs no filtering.
        foreach(_ra8_univ_frag
                "/libs/ra8_core/src/"
                "/libs/ra8_hal/src/"
                "/libs/ra8_net_pal/src/"
                "/libs/ra8_usb_pal/src/"
                "/libs/ra8_board_${_RA8_APP_BOARD}/src/"
                "/src/secure_app/")
            list(FILTER _ra8_lib_extra EXCLUDE REGEX "${_ra8_univ_frag}")
        endforeach()
    endif()

    add_executable(${_ra8_elf}
        ${_ra8_src}
        ${_ra8_lib_core} ${_ra8_lib_hal} ${_ra8_lib_net_pal} ${_ra8_lib_usb_pal}
        ${_ra8_lib_nsc} ${_ra8_lib_board} ${_ra8_secure_app}
        ${_ra8_lib_extra} ${_ra8_lib_extra_sim})

    if(_ra8_use_shared_archive)
        target_link_libraries(${_ra8_elf} PRIVATE
            -Wl,--whole-archive ${_ra8_shared_archive} -Wl,--no-whole-archive)
        set_property(TARGET ${_ra8_elf} APPEND PROPERTY
            LINK_DEPENDS ${_ra8_shared_archive})
    endif()

    if(_ra8_lib_extra_sim)
        set_source_files_properties(${_ra8_lib_extra_sim}
            PROPERTIES COMPILE_DEFINITIONS "RA8_SIMULATOR_MODE")
    endif()

    # Route stb_truetype's allocations through the heap-free arena (see above).
    # The vendored header trips only the style classes suppressed by the SOUP
    # -Wno set above (bad-function-cast, cast-qual, double-promotion,
    # unused-parameter); -Werror stays in force for the memory-safety classes.
    if(_ra8_stb_impl)
        set_property(SOURCE ${_ra8_stb_impl} APPEND PROPERTY COMPILE_OPTIONS
            ${_ra8_soup_wno_common} ${_ra8_soup_wno_c}
            -include "${RA8_REPO_ROOT}/libs/ra8_reflow/inc/ra8_stbtt_alloc.h"
            "-DSTBTT_malloc(x,u)=ra8_stbtt_malloc(x)"
            "-DSTBTT_free(x,u)=ra8_stbtt_free(x)")
        # stb_truetype pulls in sqrt/floor/ceil from the math library.
        target_link_libraries(${_ra8_elf} PRIVATE m)
    endif()

    # Same treatment for the stb_image single-TU build: a vendored TU that trips
    # the SOUP style classes (cast-align, cast-qual, double-promotion,
    # duplicated-branches, missing-declarations/-prototypes) only. -Werror stays
    # in force for the memory-safety classes on this attacker-facing decoder.
    # The STBI_* allocator macros are defined inside stb_image_impl.c itself.
    if(_ra8_stb_img_impl)
        set_property(SOURCE ${_ra8_stb_img_impl} APPEND PROPERTY COMPILE_OPTIONS
            ${_ra8_soup_wno_common} ${_ra8_soup_wno_c})
    endif()

    # The vendored miniz.c (C) / tinyxml2.cpp (C++) trip only the SOUP style
    # classes; suppress those with the narrow -Wno set so -Werror stays in force
    # for the memory-safety classes (the first-party ra8_epub .c / .cpp shim still
    # go through the full warning set). The C-only names would be a hard -Werror
    # in g++, so they are gated to the C TU via $<COMPILE_LANGUAGE:C>.
    # MINIZ_NO_STDIO/NO_TIME must be set for EVERY TU that includes miniz.h (the
    # vendored miniz.c *and* ra8_epub's TUs) so the header ABI matches -- miniz.c
    # references utime()/fopen() (absent on bare-metal newlib) only under the
    # default config, and a mismatched config between miniz.c and ra8_epub corrupts
    # the reader. ra8_epub uses only the in-memory reader, so the file path is
    # never needed.
    if(_ra8_epub_vendor)
        set_property(SOURCE ${_ra8_epub_vendor} APPEND PROPERTY COMPILE_OPTIONS
            ${_ra8_soup_wno_common}
            "$<$<COMPILE_LANGUAGE:C>:${_ra8_soup_wno_c}>")
        target_compile_definitions(${_ra8_elf} PRIVATE MINIZ_NO_STDIO MINIZ_NO_TIME)
    endif()

    # miniz-only apps: same narrow vendored-TU warning suppression + config
    # defines, so miniz.c and every first-party TU that includes miniz.h share one
    # ABI. NO_ARCHIVE_APIS drops the malloc-using ZIP reader the firmware never
    # calls, leaving the heap-free tinfl DEFLATE core (driven from a static
    # decompressor). miniz.c is C, so it takes the C-only names too.
    if(_ra8_miniz_vendor)
        set_property(SOURCE ${_ra8_miniz_vendor} APPEND PROPERTY COMPILE_OPTIONS
            ${_ra8_soup_wno_common} ${_ra8_soup_wno_c})
        target_compile_definitions(${_ra8_elf} PRIVATE
            MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_ARCHIVE_APIS)
    endif()

    # The vendored libwebp decoder (SOUP): a 60+ TU codec (VP8/VP8L + the
    # per-arch SIMD stubs) whose per-TU narrow -Wno tuning is not tractable, so
    # it takes a blanket -w + -fno-strict-aliasing (it type-puns through byte
    # buffers), matching tests/CMakeLists.txt. Its attacker-facing memory-safety
    # net is the ASan/UBSan libFuzzer harness (fuzz_ra8_webp), not -Werror.
    # -DRA8_WEBP_USE_ARENA activates the utils.c RA8 LOCAL PATCH that routes the
    # allocator through the heap-free ra8_webp bump arena (see docs/SOUP/libwebp.md).
    if(_ra8_webp_vendor)
        ra8_webp_apply_soup_flags(${_ra8_webp_vendor})
    endif()

    # The vendored xz-embedded decoder (SOUP): blanket -w + -fno-strict-aliasing
    # matching tests/CMakeLists.txt. Its attacker-facing memory-safety net is
    # the ASan/UBSan libFuzzer harness (fuzz_ra8_unarch_xz) plus the bounded
    # first-party wrapper (ra8_unarch_xz.c), which is held to the full warning
    # bar and charges every decode against the decompression-limits policy.
    if(_ra8_xz_vendor)
        set_source_files_properties(${_ra8_xz_vendor} PROPERTIES COMPILE_OPTIONS
            "-w;-fno-strict-aliasing")
    endif()

    ra8_target_enable_project_warnings(${_ra8_elf} STACK_USAGE_BYTES ${_RA8_APP_STACK_BYTES})
    target_compile_options(${_ra8_elf} PRIVATE -fshort-enums)

    # Vendored RTOS / middleware headers trip several strict-warning gates we
    # apply to first-party code (CHAR* params, redundant decls, casts,
    # redefined macros). Relax the gate for apps that pull them in; the rest
    # of the codebase still gets the full -Werror set.
    if(_RA8_APP_USES)
        target_compile_options(${_ra8_elf} PRIVATE
            -Wno-error=discarded-qualifiers
            -Wno-error=cast-qual
            -Wno-error=cast-align
            -Wno-error=redundant-decls
            -Wno-error=missing-prototypes
            -Wno-error=builtin-macro-redefined
            -Wno-error)
    endif()

    if(RA8_TRUSTZONE_ENABLE)
        target_compile_definitions(${_ra8_elf} PRIVATE RA8_TRUSTZONE_ENABLE)
        target_compile_options(${_ra8_elf} PRIVATE -mcmse)
        target_link_options(${_ra8_elf} PRIVATE -mcmse)
    endif()

    # Compile the insecure placeholder crypto only when explicitly opted in
    # (issue #180). Left OFF, the guarded #else branches fail closed.
    if(RA8_INSECURE_STUB_CRYPTO)
        target_compile_definitions(${_ra8_elf} PRIVATE RA8_INSECURE_STUB_CRYPTO)
    endif()

    target_include_directories(${_ra8_elf} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/inc
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${RA8_REPO_ROOT}/src
        ${RA8_REPO_ROOT}/src/inc
        ${RA8_REPO_ROOT}/src/secure_app
        ${RA8_REPO_ROOT}/src/secure_app/inc
        ${RA8_REPO_ROOT}/libs/ra8_core/inc
        ${RA8_REPO_ROOT}/libs/ra8_hal/inc
        ${RA8_REPO_ROOT}/libs/ra8_net_pal/inc
        ${RA8_REPO_ROOT}/libs/ra8_usb_pal/inc
        ${RA8_REPO_ROOT}/libs/ra8_nsc/inc
        ${_ra8_board_dir}/inc
        ${_ra8_lib_inc}
        ${_ra8_extra_inc})

    # Each middleware <m> ships a board-port interface library <m>_port_<bus>
    # that carries the RA-specific bridge headers; link it alongside the core.
    set(_ra8_port_lib_threadx "")
    set(_ra8_port_lib_usbx    usbx_port_ra8_usb)
    set(_ra8_port_lib_netxduo netxduo_port_ra8_eth)
    set(_ra8_port_lib_mbedtls mbedtls_port_ra8_config)
    set(_ra8_port_lib_filex   filex_port_ra8_sdhi)
    set(_ra8_port_lib_levelx  levelx_port_ra8_xspi)
    set(_ra8_port_lib_nimble  nimble_port_threadx)

    # Link the middleware interface libraries + pull in their bridge sources.
    foreach(_ra8_use ${_RA8_APP_USES})
        string(TOUPPER "${_ra8_use}" _ra8_use_up)
        if(TARGET ${_ra8_use})
            target_link_libraries(${_ra8_elf} PRIVATE ${_ra8_use})
        endif()
        if(_ra8_port_lib_${_ra8_use} AND TARGET ${_ra8_port_lib_${_ra8_use}})
            target_link_libraries(${_ra8_elf} PRIVATE ${_ra8_port_lib_${_ra8_use}})
        endif()
        get_property(_ra8_port GLOBAL PROPERTY RA8_${_ra8_use_up}_PORT_SOURCES)
        if(_ra8_port)
            target_sources(${_ra8_elf} PRIVATE ${_ra8_port})
        endif()
    endforeach()

    target_link_options(${_ra8_elf} PRIVATE -T${_ra8_linker} -Wl,--Map=${_RA8_APP_NAME}.map)
    set_target_properties(${_ra8_elf} PROPERTIES LINK_DEPENDS ${_ra8_linker})

    add_custom_command(TARGET ${_ra8_elf} POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${_ra8_elf}> ${_RA8_APP_NAME}.hex
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${_ra8_elf}> ${_RA8_APP_NAME}.bin
        COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${_ra8_elf}>
        COMMENT "Generating ${_RA8_APP_NAME}.hex / .bin and showing size")
endmacro()

# ============================================================================
# ra8_add_cpu1_image() -- embed a Cortex-M33 (CPU1) image into an M85 (CPU0) ELF
# ============================================================================
# Builds a second, freestanding Cortex-M33 executable from SOURCES, objcopies it
# to a raw .bin, repacks that as a relocatable `.cpu1_image` object, and links it
# into PARENT (the M85 app .elf). The app's linker_script.ld must pin
# `.cpu1_image` at ORIGIN(MRAM_CPU1) (0x020C0000) so a single SWD flash drops
# both cores' images. The M33 firmware exports `cpu1_reset_handler` and an
# 8-entry vector table at 0x020C0000; `ra8_cpu1_release()` (HUM Ch 2.9.1) starts
# it at runtime. Both RA8D2 cores are single-precision FP (fpv5-sp-d16).
#
# Generalises the copy-pasted second-executable + objcopy recipe so any app opts
# in an M33 image with one call. Cross-build only: on the host
# (RA8_SIMULATOR_MODE / __APPLE__) there is no arm-none-eabi toolchain, so this is
# a no-op and `make test` keeps building the M85 side alone.
#
# Usage:
#   ra8_add_cpu1_image(
#     PARENT   blink_m33            # M85 app target base name (-> <PARENT>.elf)
#     NAME     blink_m33_cpu1       # CPU1 target base name (-> <NAME>.elf/.bin)
#     SOURCES  cpu1_main.c          # M33 sources (relative to the app dir)
#     LINKER   linker_script_cpu1.ld  # optional; defaults to this name
#     INCLUDES ${EXTRA_INC} ...     # optional extra include dirs
#   )
function(ra8_add_cpu1_image)
    cmake_parse_arguments(C1 "" "PARENT;NAME;LINKER" "SOURCES;INCLUDES" ${ARGN})

    # Host build: no cross toolchain, so skip the M33 image entirely.
    if(NOT (CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_SYSTEM_NAME STREQUAL "Generic"))
        return()
    endif()
    if(NOT C1_PARENT)
        message(FATAL_ERROR "ra8_add_cpu1_image(): PARENT (the M85 app target) is required")
    endif()
    if(NOT C1_NAME)
        set(C1_NAME ${C1_PARENT}_cpu1)
    endif()
    if(NOT C1_LINKER)
        set(C1_LINKER linker_script_cpu1.ld)
    endif()

    set(_c1_ld ${CMAKE_CURRENT_SOURCE_DIR}/${C1_LINKER})
    set(_c1_srcs "")
    foreach(_s ${C1_SOURCES})
        list(APPEND _c1_srcs ${CMAKE_CURRENT_SOURCE_DIR}/${_s})
    endforeach()

    # The M33 image: -mcpu=cortex-m33, no M85 startup (own cpu1_reset_handler),
    # freestanding, size-optimised to fit the 256 KiB MRAM_CPU1 region.
    add_executable(${C1_NAME}.elf ${_c1_srcs})
    target_compile_definitions(${C1_NAME}.elf PRIVATE RA8_BUILD_FOR_CPU1)
    target_compile_options(${C1_NAME}.elf PRIVATE
        -mcpu=cortex-m33 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16
        -ffreestanding -fno-builtin -fshort-enums -Os -g3)

    # The per-app Cortex-M33 entry (the helper's own SOURCES, e.g. cpu1_main.c)
    # was escaping the project warning / -Werror / stack-usage bar that every
    # M85 TU receives via ra8_target_enable_project_warnings(): this recipe set
    # only the cpu flags + -Os, so half the dual-core product compiled with zero
    # of -Wall / -Wextra / -Werror / -Wshadow / -Wstack-usage and the M33 side
    # emitted no .su stack data. Apply the same first-party warning set the M85
    # build uses (kept in step with cmake/ra8_warnings.cmake) plus -Wstack-usage
    # + -fstack-usage so cpu1_main.c is held to the same safety bar and feeds the
    # .su aggregator (scripts/utils/stack_usage_check.py).
    #
    # Applied PER-SOURCE to the helper SOURCES -- NOT target-level -- on purpose:
    # an app may bolt extra translation units onto the CPU1 elf with its own
    # target_sources() (e.g. compile_on_m33 adds the rabook XHTML pipeline plus
    # vendored miniz.c / tinyxml2.cpp with their own -w). A target-level -Werror
    # would force those onto the first-party bar (and -Wstack-usage onto the
    # multi-KiB XHTML walker frames), reddening the build. Per-source scoping
    # holds the lines this helper owns to the bar without touching the app's
    # bolted-on sources. TODO(T1-09): extend the bar to app-added first-party M33
    # TUs (ra8_gfx / ra8_ipc / ra8_rabook on the M33 side) via per-source opt-in in
    # those apps' own CMakeLists; those TUs are still -Werror-gated in the M85
    # builds where they are also compiled.
    set_source_files_properties(${_c1_srcs}
        PROPERTIES COMPILE_OPTIONS
        "-Wall;-Wextra;-Werror;-Wcast-qual;-Wcast-align;-Wdouble-promotion;-Wformat=2;-Wpointer-arith;-Wshadow;-Wundef;-Wvla;-Wwrite-strings;-Wbad-function-cast;-Wmissing-declarations;-Wmissing-prototypes;-Wnested-externs;-Wold-style-definition;-Wredundant-decls;-Wstrict-prototypes;-Wduplicated-branches;-Wduplicated-cond;-Wformat-overflow=2;-Wformat-truncation=2;-Wlogical-op;-Wstack-usage=2048;-fstack-usage")
    target_link_options(${C1_NAME}.elf PRIVATE
        -mcpu=cortex-m33 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16
        -nostartfiles -T${_c1_ld} -Wl,--Map=${C1_NAME}.map)
    target_include_directories(${C1_NAME}.elf PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${RA8_REPO_ROOT}/libs/ra8_core/inc
        ${C1_INCLUDES})
    set_target_properties(${C1_NAME}.elf PROPERTIES LINK_DEPENDS ${_c1_ld})
    add_custom_command(TARGET ${C1_NAME}.elf POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${C1_NAME}.elf> ${C1_NAME}.hex
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${C1_NAME}.elf> ${C1_NAME}.bin
        COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${C1_NAME}.elf>
        COMMENT "Generating ${C1_NAME}.hex / .bin (Cortex-M33 image) and showing size")

    # Repack the CPU1 .bin as a relocatable `.cpu1_image` object and link it into
    # PARENT; the app linker script pins `.cpu1_image` at ORIGIN(MRAM_CPU1) so one
    # .hex spans MRAM (0x02000000) and MRAM_CPU1 (0x020C0000).
    set(_c1_bin ${CMAKE_CURRENT_BINARY_DIR}/${C1_NAME}.bin)
    set(_c1_obj ${CMAKE_CURRENT_BINARY_DIR}/${C1_NAME}_blob.o)
    add_custom_command(OUTPUT ${_c1_obj}
        COMMAND ${CMAKE_OBJCOPY}
            -I binary -O elf32-littlearm -B arm
            --rename-section .data=.cpu1_image,alloc,load,readonly,contents
            ${_c1_bin} ${_c1_obj}
        DEPENDS ${C1_NAME}.elf
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Packing ${C1_NAME}.bin as relocatable .cpu1_image object")
    target_sources(${C1_PARENT}.elf PRIVATE ${_c1_obj})
    set_source_files_properties(${_c1_obj} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    add_dependencies(${C1_PARENT}.elf ${C1_NAME}.elf)
endfunction()
