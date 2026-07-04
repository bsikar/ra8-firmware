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
#   - linker_script.ld               : the app's local copy if it exists
#       (divergent maps: dual-core, TrustZone, bootloader banks), else the
#       shared canonical single-core map libs/ra_board_ek_ra8d2/ld/linker_script.ld
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

    # ---- insecure placeholder-crypto opt-in (issue #180) ------------------
    # Several secure-side TUs (src/secure_app/{secure_trng,key_import,key_vault}.c
    # and libs/ra_hal/src/ra_rsip_key_injection.c) ship an INSECURE placeholder
    # body (a deterministic PRNG "TRNG", a forgeable key-import MAC, a plain-SRAM
    # key vault, a non-cryptographic RSIP key-wrap) that is only safe under host
    # simulation or an explicitly-declared dev/eval image. Each such body is
    # wrapped in `#if defined(RA_INSECURE_STUB_CRYPTO) || defined(RA_SIMULATOR_MODE)`
    # and its `#else` fails closed (every entry point returns a hard error). This
    # option is OFF by default so a release/HIL firmware image that forgets to
    # swap in a real crypto backend fails closed instead of silently shipping the
    # stub. A dev/eval firmware image opts in with -DRA_INSECURE_STUB_CRYPTO=ON.
    option(RA_INSECURE_STUB_CRYPTO "compile insecure placeholder crypto (host/sim only)" OFF)

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

    # ra_reflow's glyph rasteriser (ra_reflow_render.c, #164) caches glyph bitmaps
    # through the Layer-3 ra_glyph_atlas, and ra_book's paged accessor
    # (ra_book_paged.c / ra_book_xhtml.c, #163) reads books through the ra_vmem
    # page cache -- both in libs/ra_mem. ra_mem depends only on ra_core, so wire
    # it in automatically for ra_reflow / ra_book consumers (mirroring the stb
    # special-case above), unless the app already lists ra_mem in LIBS -- in which
    # case the loop above already globbed it.
    if((("ra_reflow" IN_LIST _RA_APP_LIBS) OR ("ra_book" IN_LIST _RA_APP_LIBS)) AND
       (NOT "ra_mem" IN_LIST _RA_APP_LIBS))
        file(GLOB_RECURSE _ra_lib_mem CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_mem/src/*.c)
        list(APPEND _ra_lib_extra ${_ra_lib_mem})
        list(APPEND _ra_lib_inc ${RA_REPO_ROOT}/libs/ra_mem/inc)
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
    set(_ra_soup_wno_common
        -Wno-cast-qual          # miniz / stb cast away const on byte buffers
        -Wno-cast-align         # stb_image casts to a wider alignment
        -Wno-double-promotion   # stb float -> double in its math hooks
        -Wno-unused-parameter   # stb / tinyxml2 unused formals
        -Wno-type-limits        # miniz range-limited comparisons
        -Wno-duplicated-branches   # stb_image identical if/else arms
        -Wno-missing-declarations  # stb_image extern helpers with no prior decl
        # stb_image's GIF path (stbi__gif_load / stbi__load_gif_main) puts ~34 KiB
        # of frame buffers on the stack -- far over the per-app -Wstack-usage=N
        # budget, but an inherent property of the vendored decoder we cannot shrink
        # without editing SOUP. The blanket -w hid this too; suppress only the
        # warning here. -fstack-usage still emits the .su data, so the real
        # project-wide stack-bound proof (scripts/utils/stack_usage_check.py over
        # the ARM .su files) still sees these frames.
        -Wno-stack-usage)
    set(_ra_soup_wno_c
        -Wno-bad-function-cast     # stb_truetype casts a call result
        -Wno-missing-prototypes)   # stb globals with no prior prototype

    # linker_script.ld: app dir if present, else the shared canonical single-core
    # map. 125 apps carried a byte-identical script; the fallback lets them build
    # from one source so a region-map change touches one file, not 125 (T3-01).
    # Apps with a divergent map (dual-core, TrustZone slots, bootloader banks)
    # keep their own linker_script.ld and override the default.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/linker_script.ld")
        set(_ra_linker ${CMAKE_CURRENT_SOURCE_DIR}/linker_script.ld)
    else()
        set(_ra_linker ${RA_REPO_ROOT}/libs/ra_board_ek_ra8d2/ld/linker_script.ld)
    endif()
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
    # The vendored header trips only the style classes suppressed by the SOUP
    # -Wno set above (bad-function-cast, cast-qual, double-promotion,
    # unused-parameter); -Werror stays in force for the memory-safety classes.
    if(_ra_stb_impl)
        set_property(SOURCE ${_ra_stb_impl} APPEND PROPERTY COMPILE_OPTIONS
            ${_ra_soup_wno_common} ${_ra_soup_wno_c}
            -include "${RA_REPO_ROOT}/libs/ra_reflow/inc/ra_stbtt_alloc.h"
            "-DSTBTT_malloc(x,u)=ra_stbtt_malloc(x)"
            "-DSTBTT_free(x,u)=ra_stbtt_free(x)")
        # stb_truetype pulls in sqrt/floor/ceil from the math library.
        target_link_libraries(${_ra_elf} PRIVATE m)
    endif()

    # Same treatment for the stb_image single-TU build: a vendored TU that trips
    # the SOUP style classes (cast-align, cast-qual, double-promotion,
    # duplicated-branches, missing-declarations/-prototypes) only. -Werror stays
    # in force for the memory-safety classes on this attacker-facing decoder.
    # The STBI_* allocator macros are defined inside stb_image_impl.c itself.
    if(_ra_stb_img_impl)
        set_property(SOURCE ${_ra_stb_img_impl} APPEND PROPERTY COMPILE_OPTIONS
            ${_ra_soup_wno_common} ${_ra_soup_wno_c})
    endif()

    # The vendored miniz.c (C) / tinyxml2.cpp (C++) trip only the SOUP style
    # classes; suppress those with the narrow -Wno set so -Werror stays in force
    # for the memory-safety classes (the first-party ra_epub .c / .cpp shim still
    # go through the full warning set). The C-only names would be a hard -Werror
    # in g++, so they are gated to the C TU via $<COMPILE_LANGUAGE:C>.
    # MINIZ_NO_STDIO/NO_TIME must be set for EVERY TU that includes miniz.h (the
    # vendored miniz.c *and* ra_epub's TUs) so the header ABI matches -- miniz.c
    # references utime()/fopen() (absent on bare-metal newlib) only under the
    # default config, and a mismatched config between miniz.c and ra_epub corrupts
    # the reader. ra_epub uses only the in-memory reader, so the file path is
    # never needed.
    if(_ra_epub_vendor)
        set_property(SOURCE ${_ra_epub_vendor} APPEND PROPERTY COMPILE_OPTIONS
            ${_ra_soup_wno_common}
            "$<$<COMPILE_LANGUAGE:C>:${_ra_soup_wno_c}>")
        target_compile_definitions(${_ra_elf} PRIVATE MINIZ_NO_STDIO MINIZ_NO_TIME)
    endif()

    # miniz-only apps: same narrow vendored-TU warning suppression + config
    # defines, so miniz.c and every first-party TU that includes miniz.h share one
    # ABI. NO_ARCHIVE_APIS drops the malloc-using ZIP reader the firmware never
    # calls, leaving the heap-free tinfl DEFLATE core (driven from a static
    # decompressor). miniz.c is C, so it takes the C-only names too.
    if(_ra_miniz_vendor)
        set_property(SOURCE ${_ra_miniz_vendor} APPEND PROPERTY COMPILE_OPTIONS
            ${_ra_soup_wno_common} ${_ra_soup_wno_c})
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

    # Compile the insecure placeholder crypto only when explicitly opted in
    # (issue #180). Left OFF, the guarded #else branches fail closed.
    if(RA_INSECURE_STUB_CRYPTO)
        target_compile_definitions(${_ra_elf} PRIVATE RA_INSECURE_STUB_CRYPTO)
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

# ============================================================================
# ra_add_cpu1_image() -- embed a Cortex-M33 (CPU1) image into an M85 (CPU0) ELF
# ============================================================================
# Builds a second, freestanding Cortex-M33 executable from SOURCES, objcopies it
# to a raw .bin, repacks that as a relocatable `.cpu1_image` object, and links it
# into PARENT (the M85 app .elf). The app's linker_script.ld must pin
# `.cpu1_image` at ORIGIN(MRAM_CPU1) (0x020C0000) so a single SWD flash drops
# both cores' images. The M33 firmware exports `cpu1_reset_handler` and an
# 8-entry vector table at 0x020C0000; `ra_cpu1_release()` (HUM Ch 2.9.1) starts
# it at runtime. Both RA8D2 cores are single-precision FP (fpv5-sp-d16).
#
# Generalises the copy-pasted second-executable + objcopy recipe so any app opts
# in an M33 image with one call. Cross-build only: on the host
# (RA_SIMULATOR_MODE / __APPLE__) there is no arm-none-eabi toolchain, so this is
# a no-op and `make test` keeps building the M85 side alone.
#
# Usage:
#   ra_add_cpu1_image(
#     PARENT   blink_m33            # M85 app target base name (-> <PARENT>.elf)
#     NAME     blink_m33_cpu1       # CPU1 target base name (-> <NAME>.elf/.bin)
#     SOURCES  cpu1_main.c          # M33 sources (relative to the app dir)
#     LINKER   linker_script_cpu1.ld  # optional; defaults to this name
#     INCLUDES ${EXTRA_INC} ...     # optional extra include dirs
#   )
function(ra_add_cpu1_image)
    cmake_parse_arguments(C1 "" "PARENT;NAME;LINKER" "SOURCES;INCLUDES" ${ARGN})

    # Host build: no cross toolchain, so skip the M33 image entirely.
    if(NOT (CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_SYSTEM_NAME STREQUAL "Generic"))
        return()
    endif()
    if(NOT C1_PARENT)
        message(FATAL_ERROR "ra_add_cpu1_image(): PARENT (the M85 app target) is required")
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
    target_compile_definitions(${C1_NAME}.elf PRIVATE RA_BUILD_FOR_CPU1)
    target_compile_options(${C1_NAME}.elf PRIVATE
        -mcpu=cortex-m33 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16
        -ffreestanding -fno-builtin -fshort-enums -Os -g3)

    # The per-app Cortex-M33 entry (the helper's own SOURCES, e.g. cpu1_main.c)
    # was escaping the project warning / -Werror / stack-usage bar that every
    # M85 TU receives via ra_target_enable_project_warnings(): this recipe set
    # only the cpu flags + -Os, so half the dual-core product compiled with zero
    # of -Wall / -Wextra / -Werror / -Wshadow / -Wstack-usage and the M33 side
    # emitted no .su stack data. Apply the same first-party warning set the M85
    # build uses (kept in step with cmake/ra_warnings.cmake) plus -Wstack-usage
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
    # TUs (ra_gfx / ra_ipc / ra_rabook on the M33 side) via per-source opt-in in
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
        ${RA_REPO_ROOT}/libs/ra_core/inc
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
