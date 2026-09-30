# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# cmake/ra8_app/sources.cmake -- which translation units an app compiles.
#
# SOURCED, NEVER EXECUTED on its own. cmake/ra8_add_app.cmake includes this
# file once and calls the macro below from inside ra8_add_app().
#
# It is a macro(), not a function(): CMake functions create a new variable
# scope, so every _ra8_* variable this sets would vanish on return. A macro is
# textual substitution and behaves exactly as the inline code it replaces --
# which is what makes this split behaviour-preserving rather than a rewrite.
#
# Everything that decides an app's SOURCE LIST: its own main and boot files,
# app-local helpers, the LIBS expansion (first-party libraries plus the
# vendored SOUP each one drags in), and the narrow warning-suppression sets
# those vendored TUs need. The per-TU compile options that CONSUME those sets
# live in vendored.cmake, and run after the executable exists.
# Collect every source this app compiles.
#
# Suppression rationale: this macro is the library/source matrix; splitting it would scatter it.
# cmake-lint: disable=R0912,R0915
#
# The branch and statement ceilings are waived here for the same reason
# ra8_add_app() waives them, and the reason survives the move: these branches
# ARE the library matrix -- one arm per library an app can name in LIBS, plus
# the vendored SOUP each of those drags in. Splitting further would not remove
# a branch, it would scatter one table across several files, and "which
# sources does LIBS reflow pull in" would stop being answerable by reading
# one list. The waiver is per-file; the global ceilings in .cmake-format.yaml
# stay at cmakelang defaults so no other listfile inherits it.
# #908: the LIBS expansions below only ever glob *.c, so a library that ships
# its implementation in anything else contributes NO object code and says
# nothing about it. Whether that surfaces as an undefined reference or as
# quietly-missing behaviour depends on how the app reaches the library, and
# neither failure names the cause -- which is exactly why every Zig port so far
# has had to keep its C implementation alongside the new one. Stop at configure
# time instead: if the library directory holds sources this build cannot
# compile and produced no objects, say so. A genuinely header-only library has
# no sources at all and is unaffected.
#
# A function, not a macro: it needs no variable to survive the return, and the
# two call sites (LIBS and OFF_TARGET_LIBS) are otherwise identical loops whose
# only difference is which keyword the app wrote.
#
# A library whose implementation has finished migrating is the one case that
# looks exactly like the defect and is not one: src holds .zig and no .c, so
# the *.c glob is empty, but the objects are not missing -- they arrive in the
# Zig static archive the caller has just registered in _ra8_lib_zig. That is
# the same reasoning the ra8_net_pal block below states in prose; _has_archive
# carries it to the two keyword loops so a fully-ported library can be named in
# LIBS at all.
function(
  _ra8_app_require_compilable_lib
  _lib
  _path
  _why
  _globbed
  _has_archive
)
  if(_globbed
     OR NOT _path
     OR _has_archive
  )
    return()
  endif()
  file(
    GLOB_RECURSE
    _uncompiled
    CONFIGURE_DEPENDS
    ${_path}/src/*.zig
    ${_path}/src/*.cpp
    ${_path}/src/*.cc
    ${_path}/src/*.S
    ${_path}/*.zig
  )
  if(NOT _uncompiled)
    return()
  endif()
  list(JOIN _uncompiled "\n    " _uncompiled_pretty)
  message(
    FATAL_ERROR
      "ra8_add_app(): ${_RA8_APP_NAME} ${_why}, but ${_path}/src holds no C "
      "sources and this expansion only compiles *.c, so ${_lib} would "
      "contribute no object code (issue #908). Sources found but not "
      "compiled:\n    ${_uncompiled_pretty}\n  Wire the non-C sources into "
      "the app build before removing the C implementation."
  )
endfunction()

macro(_ra8_app_collect_sources)
  # ---- sources: per-app main, shared-or-local boot ----------------------
  #
  # Each boot file resolves in three rungs: the app's own src/<file>.c, then
  # the board's src/boot/<BOOT_PROFILE>/<file>.c, then the board default
  # src/boot/<file>.c. The middle rung exists because a boot file is often
  # shared by a FAMILY of apps rather than by all of them or by one: two apps
  # hand off to a Non-Secure USB image and two are deliberately secure-only,
  # and each pair carried a byte-identical trustzone_init.c differing only in
  # the @file line (#742). BOOT_PROFILE lets the pair name one shared copy
  # without promoting it to the board default, which would change every other
  # app on the board.
  #
  # A profile directory holds only the boot files that profile overrides; any
  # it omits fall through to the board default. The board library glob below
  # excludes /src/boot/ wholesale, so profile files are compiled into the apps
  # that select them and into nothing else.
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/main.c")
    message(FATAL_ERROR "${CMAKE_CURRENT_SOURCE_DIR} must provide src/main.c")
  endif()
  file(GLOB _ra8_app_local CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
  set(_ra8_primary "${CMAKE_CURRENT_SOURCE_DIR}/src/main.c")
  list(REMOVE_ITEM _ra8_app_local "${_ra8_primary}")
  set(_ra8_src "${_ra8_primary}")
  foreach(
    _ra8_boot
    vector_table.c
    system_init.c
    secure_exception.c
    nmi_exception.c
    trustzone_init.c
  )
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/${_ra8_boot}")
      list(APPEND _ra8_src "${CMAKE_CURRENT_SOURCE_DIR}/src/${_ra8_boot}")
      list(REMOVE_ITEM _ra8_app_local "${CMAKE_CURRENT_SOURCE_DIR}/src/${_ra8_boot}")
    elseif(_RA8_APP_BOOT_PROFILE
           AND EXISTS "${_ra8_board_dir}/src/boot/${_RA8_APP_BOOT_PROFILE}/${_ra8_boot}"
    )
      list(APPEND _ra8_src "${_ra8_board_dir}/src/boot/${_RA8_APP_BOOT_PROFILE}/${_ra8_boot}")
    else()
      list(APPEND _ra8_src "${_ra8_board_dir}/src/boot/${_ra8_boot}")
    endif()
  endforeach()

  # Every app-local implementation lives under src/ and is compiled into the
  # app. Public/local interfaces live under inc/; module-private headers may
  # remain beside their implementation under src/. Auxiliary-image sources
  # are named explicitly so they can coexist under src/ without leaking into
  # the primary image's target.
  foreach(_ra8_aux ${_RA8_APP_AUX_SRCS})
    if(IS_ABSOLUTE "${_ra8_aux}")
      set(_ra8_aux_abs "${_ra8_aux}")
    else()
      get_filename_component(_ra8_aux_abs "${CMAKE_CURRENT_SOURCE_DIR}/${_ra8_aux}" ABSOLUTE)
    endif()
    if(NOT EXISTS "${_ra8_aux_abs}")
      message(FATAL_ERROR "AUX_SRCS entry does not exist: ${_ra8_aux}")
    endif()
    list(REMOVE_ITEM _ra8_app_local "${_ra8_aux_abs}")
  endforeach()
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

  file(GLOB_RECURSE _ra8_lib_core CONFIGURE_DEPENDS ${RA8_REPO_ROOT}/libs/ra8_core/src/*.c)
  _ra8_app_require_compilable_lib(
    ra8_core
    "${RA8_REPO_ROOT}/libs/ra8_core"
    "links ra8_core into every app"
    "${_ra8_lib_core}"
    ""
  )
  file(GLOB_RECURSE _ra8_lib_hal CONFIGURE_DEPENDS ${RA8_REPO_ROOT}/libs/ra8_hal/src/*.c)
  _ra8_app_require_compilable_lib(
    ra8_hal
    "${RA8_REPO_ROOT}/libs/ra8_hal"
    "links ra8_hal into every app"
    "${_ra8_lib_hal}"
    ""
  )
  # ra8_net_pal has no C sources: the library is Zig (#1039) and its objects
  # come from the Zig static archive, linked separately. Left unglobbed and
  # deliberately outside the #908 guard, which exists to catch a library whose
  # objects silently vanish; this one's have a link path of their own.
  # ra8_usb_pal has no C sources either: the PAL core, the descriptor builders
  # and the compose facade are Zig (#766) and its objects come from the Zig
  # static archive, linked separately. Left unglobbed and deliberately outside
  # the #908 guard, for the same reason as ra8_net_pal above.
  file(GLOB_RECURSE _ra8_lib_board CONFIGURE_DEPENDS ${_ra8_board_dir}/src/*.c)
  _ra8_app_require_compilable_lib(
    "ra8_board_${_RA8_APP_BOARD}"
    "${_ra8_board_dir}"
    "builds for BOARD ${_RA8_APP_BOARD}"
    "${_ra8_lib_board}"
    ""
  )
  list(
    FILTER
    _ra8_lib_board
    EXCLUDE
    REGEX
    "/src/boot/"
  )
  file(GLOB_RECURSE _ra8_secure_app CONFIGURE_DEPENDS ${RA8_REPO_ROOT}/libs/ra8_secure_app/src/*.c)
  _ra8_app_require_compilable_lib(
    ra8_secure_app
    "${RA8_REPO_ROOT}/libs/ra8_secure_app"
    "links ra8_secure_app into every app"
    "${_ra8_secure_app}"
    ""
  )
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
    _ra8_app_require_compilable_lib(
      ra8_nsc
      "${RA8_REPO_ROOT}/libs/ra8_nsc"
      "links the ra8_nsc veneers"
      "${_ra8_lib_nsc}"
      ""
    )
  endif()

  # Extra first-party libraries (plain + off-target).
  set(_ra8_lib_extra "")
  set(_ra8_lib_extra_off_target "")
  # Migrated (Zig) libraries, as "<lib>|<path>" entries.
  set(_ra8_lib_zig "")
  # ra8_net_pal is part of every app's universal source set, so applications
  # do not normally name it in LIBS. Once its primary C implementation is
  # gone, register the replacement archive here just as the LIBS loop below
  # does for explicitly selected migrated libraries.
  set(_ra8_net_pal_path "${RA8_REPO_ROOT}/libs/ra8_net_pal")
  if(EXISTS "${_ra8_net_pal_path}/build.zig" AND NOT EXISTS
                                                 "${_ra8_net_pal_path}/src/ra8_net_pal.c"
  )
    list(APPEND _ra8_lib_zig "ra8_net_pal|${_ra8_net_pal_path}")
  endif()
  set(_ra8_lib_inc "")
  foreach(_ra8_lib ${_RA8_APP_LIBS})
    if(EXISTS "${RA8_REPO_ROOT}/libs/${_ra8_lib}")
      set(_ra8_lib_path "${RA8_REPO_ROOT}/libs/${_ra8_lib}")
    elseif(EXISTS "${RA8_REPO_ROOT}/apps/shared_libs/${_ra8_lib}")
      set(_ra8_lib_path "${RA8_REPO_ROOT}/apps/shared_libs/${_ra8_lib}")
    else()
      set(_ra8_lib_path "")
    endif()
    if(_ra8_lib_path)
      file(GLOB_RECURSE _ra8_lib_one CONFIGURE_DEPENDS ${_ra8_lib_path}/src/*.c)
      set(_ra8_lib_has_archive "")
      if(EXISTS "${_ra8_lib_path}/build.zig" AND NOT EXISTS "${_ra8_lib_path}/src/${_ra8_lib}.c")
        # A first-half port retains its primary C implementation for ARM.
        # The later ARM flip removes that file; support C sources may remain.
        # Only then link the Zig archive beside any support C objects.
        list(APPEND _ra8_lib_zig "${_ra8_lib}|${_ra8_lib_path}")
        set(_ra8_lib_has_archive ON)
      endif()
      if(_ra8_lib MATCHES "^ra8_board_")
        # Board boot sources are image-composition fallbacks selected above.
        # A board named explicitly in LIBS must not re-add src/boot after an
        # app-local vector table or system-init override has replaced it.
        list(
          FILTER
          _ra8_lib_one
          EXCLUDE
          REGEX
          "/src/boot/"
        )
      endif()
      _ra8_app_require_compilable_lib(
        "${_ra8_lib}"
        "${_ra8_lib_path}"
        "declares LIBS ${_ra8_lib}"
        "${_ra8_lib_one}"
        "${_ra8_lib_has_archive}"
      )
      list(APPEND _ra8_lib_extra ${_ra8_lib_one})
      list(APPEND _ra8_lib_inc ${_ra8_lib_path}/inc)
    endif()
  endforeach()
  # Generated protobuf-c output contains casts required by its runtime ABI.
  # Keep warnings enabled for the handwritten client and service while
  # treating this one generated translation unit like the vendored codec.
  # The file is produced by scripts/gen/gen_ra8_media_proto.sh (protoc-c 1.5.2),
  # so neither cast can be fixed at the site without hand-editing generated
  # output. Both flags were measured on this TU under arm-none-eabi-gcc 13.3.1
  # at -O0 by removing one at a time: -Wcast-qual fires on the
  # RA8__MDL__*__INIT initialisers in ra8_media_download.pb-c.h, which cast away
  # const on protobuf_c_empty_string, and -Wcast-align fires on the
  # ProtobufCMessage* -> Ra8__Mdl__* downcasts protobuf-c's unpack API returns.
  if("ra8_c6link" IN_LIST _RA8_APP_LIBS)
    # The media RPC carries the canonical mdl_format_t in its public
    # request contract. Consumers need the declaration even when they use
    # c6link only for Wi-Fi and do not otherwise compile mdl sources.
    list(APPEND _ra8_lib_inc ${RA8_REPO_ROOT}/apps/shared_libs/mdl/inc)
    set(_ra8_media_proto_warnings
        -Wno-cast-qual # protoc-c initialisers cast away const from its empty-string singleton.
        -Wno-cast-align # protobuf-c unpack downcasts a base pointer to the generated message.
    )
    set_source_files_properties(
      ${RA8_REPO_ROOT}/libs/ra8_c6link/src/ra8_media_download.pb-c.c
      PROPERTIES COMPILE_OPTIONS "${_ra8_media_proto_warnings}"
    )
  endif()
  foreach(_ra8_lib ${_RA8_APP_OFF_TARGET_LIBS})
    if(EXISTS "${RA8_REPO_ROOT}/libs/${_ra8_lib}")
      set(_ra8_lib_path "${RA8_REPO_ROOT}/libs/${_ra8_lib}")
    elseif(EXISTS "${RA8_REPO_ROOT}/apps/shared_libs/${_ra8_lib}")
      set(_ra8_lib_path "${RA8_REPO_ROOT}/apps/shared_libs/${_ra8_lib}")
    else()
      set(_ra8_lib_path "")
    endif()
    if(_ra8_lib_path)
      file(GLOB_RECURSE _ra8_lib_one CONFIGURE_DEPENDS ${_ra8_lib_path}/src/*.c)
      set(_ra8_lib_has_archive "")
      if(EXISTS "${_ra8_lib_path}/build.zig" AND NOT EXISTS "${_ra8_lib_path}/src/${_ra8_lib}.c")
        list(APPEND _ra8_lib_zig "${_ra8_lib}|${_ra8_lib_path}")
        set(_ra8_lib_has_archive ON)
      endif()
      if(_ra8_lib MATCHES "^ra8_board_")
        list(
          FILTER
          _ra8_lib_one
          EXCLUDE
          REGEX
          "/src/boot/"
        )
      endif()
      _ra8_app_require_compilable_lib(
        "${_ra8_lib}"
        "${_ra8_lib_path}"
        "declares OFF_TARGET_LIBS ${_ra8_lib}"
        "${_ra8_lib_one}"
        "${_ra8_lib_has_archive}"
      )
      list(APPEND _ra8_lib_extra_off_target ${_ra8_lib_one})
      list(APPEND _ra8_lib_inc ${_ra8_lib_path}/inc)
    endif()
  endforeach()

  # reflow rasterises glyphs through the vendored stb_truetype. Its
  # implementation TU lives under third_party (not apps/shared_libs/reflow/src), and
  # STBTT_malloc/free must be redirected to the no-heap bump arena in
  # ra8_stbtt_alloc.c. Wire that automatically when an app pulls in reflow,
  # mirroring tests/CMakeLists.txt so app + host-test builds stay in step.
  set(_ra8_stb_impl "")
  set(_ra8_stb_img_impl "")
  if("reflow" IN_LIST _RA8_APP_LIBS)
    set(_ra8_stb_impl ${RA8_REPO_ROOT}/apps/shared_libs/third_party/stb/stb_truetype_impl.c)
    # reflow also decodes raster <img> / cover art through the vendored
    # stb_image, whose allocator is redirected to the heap-free bump arena in
    # ra8_img_arena.c. That single-TU build (stb_image_impl.c) is self-contained
    # (the STBI_* macros are defined inside it), so it needs no -include here.
    set(_ra8_stb_img_impl ${RA8_REPO_ROOT}/apps/shared_libs/third_party/stb/stb_image_impl.c)
    # The arena hooks forward to the shared decoder scratch, and reflow_image.c
    # routes WebP through the shared container sniff (#768), so both of those
    # TUs travel with the reflow sources wherever they go.
    list(APPEND _ra8_lib_extra ${_ra8_stb_impl} ${_ra8_stb_img_impl})
    # ra8_imgdec is a Zig archive now, so the scratch and the sniff arrive as
    # one library rather than two TUs compiled by path.
    list(APPEND _ra8_lib_zig "ra8_imgdec|${RA8_REPO_ROOT}/libs/ra8_imgdec")
    list(
      APPEND
      _ra8_lib_inc
      ${RA8_REPO_ROOT}/apps/shared_libs/third_party/stb
      ${RA8_REPO_ROOT}/apps/shared_libs/reflow/src
      ${RA8_REPO_ROOT}/libs/ra8_imgdec/inc
    )
  elseif("rabook_compile" IN_LIST _RA8_APP_LIBS)
    set(_ra8_stb_img_impl ${RA8_REPO_ROOT}/apps/shared_libs/third_party/stb/stb_image_impl.c)
    # ra8_rabook_raster.c routes its WebP-or-stb decision through the shared
    # container sniff (#768), the same way reflow_image.c does above, so the
    # sniff TU travels with the rabook_compile sources wherever they go.
    list(APPEND _ra8_lib_extra ${_ra8_stb_img_impl}
         ${RA8_REPO_ROOT}/apps/shared_libs/reflow/src/ra8_img_arena.c
    )
    list(APPEND _ra8_lib_zig "ra8_imgdec|${RA8_REPO_ROOT}/libs/ra8_imgdec")
    list(
      APPEND
      _ra8_lib_inc
      ${RA8_REPO_ROOT}/apps/shared_libs/third_party/stb
      ${RA8_REPO_ROOT}/apps/shared_libs/reflow/inc
      ${RA8_REPO_ROOT}/apps/shared_libs/reflow/src
      ${RA8_REPO_ROOT}/libs/ra8_imgdec/inc
    )
  endif()

  # An app may name the same vendored TU directly in EXTRA_SRCS rather than
  # reaching it through LIBS reflow / rabook_compile: media_download composes its
  # own rabook source list and does exactly that. The SOUP treatment (the narrow
  # _ra8_soup_wno_* set from issue #179, plus the -fno-strict-aliasing below) is a
  # property of the FILE, not of the route it took into the app, so recognise the
  # vendored decoder wherever it appears in this app's sources. Keying it on the
  # route left media_download compiling an attacker-facing third_party parser under
  # the first-party warning profile -- which does not build at all -- and with
  # strict aliasing on, the same miscompile class documented for miniz below.
  if(NOT _ra8_stb_img_impl)
    foreach(_ra8_src_one IN LISTS _ra8_src)
      if(_ra8_src_one MATCHES "/third_party/stb/stb_image_impl\\.c$")
        set(_ra8_stb_img_impl "${_ra8_src_one}")
        break()
      endif()
    endforeach()
  endif()

  # reflow's glyph rasteriser (reflow_render.c, #164) caches glyph bitmaps
  # through the Layer-3 ra8_glyph_atlas, and book's paged accessor
  # (book_paged.c / book_xhtml.c, #163) reads books through the ra8_vmem
  # page cache -- both in libs/ra8_mem. ra8_mem depends only on ra8_core, so wire
  # it in automatically for reflow / book consumers (mirroring the stb
  # special-case above), unless the app already lists ra8_mem in LIBS -- in which
  # case the loop above already globbed it.
  if((("reflow" IN_LIST _RA8_APP_LIBS) OR ("book" IN_LIST _RA8_APP_LIBS)) AND (NOT "ra8_mem" IN_LIST
                                                                               _RA8_APP_LIBS)
  )
    file(GLOB_RECURSE _ra8_lib_mem CONFIGURE_DEPENDS ${RA8_REPO_ROOT}/libs/ra8_mem/src/*.c)
    _ra8_app_require_compilable_lib(
      ra8_mem
      "${RA8_REPO_ROOT}/libs/ra8_mem"
      "pulls in ra8_mem for LIBS reflow/book"
      "${_ra8_lib_mem}"
      ""
    )
    list(APPEND _ra8_lib_extra ${_ra8_lib_mem})
    list(APPEND _ra8_lib_inc ${RA8_REPO_ROOT}/libs/ra8_mem/inc)
  endif()

  # ra8_camera always compiles its software-JPEG codec backend
  # (ra8_camera_codec_jpeg_sw.c), which calls ra8_jpeg_sw_encode(). Software
  # JPEG is a pure-software Domain codec and moved out of ra8_hal into its own
  # libs/ra8_jpeg, so it is no longer swept in by the always-globbed HAL. Wire
  # it transitively here for the same reason the jof block below does:
  # which image codec the camera driver dispatches to internally is the
  # driver's business, not the application's.
  if("ra8_camera" IN_LIST _RA8_APP_LIBS)
    if(NOT "ra8_jpeg" IN_LIST _RA8_APP_LIBS)
      file(GLOB_RECURSE _ra8_camera_jpeg CONFIGURE_DEPENDS ${RA8_REPO_ROOT}/libs/ra8_jpeg/src/*.c)
      _ra8_app_require_compilable_lib(
        ra8_jpeg
        "${RA8_REPO_ROOT}/libs/ra8_jpeg"
        "pulls in ra8_jpeg for LIBS ra8_camera"
        "${_ra8_camera_jpeg}"
        ""
      )
      list(APPEND _ra8_lib_extra ${_ra8_camera_jpeg})
    endif()
    list(APPEND _ra8_lib_inc ${RA8_REPO_ROOT}/libs/ra8_jpeg/inc)
  endif()

  # epub parses ZIP through vendored miniz and XML through the first-party
  # bounded xml pull reader. Only miniz is SOUP; no C++ parser is linked.
  set(_epub_vendor "")
  if("epub" IN_LIST _RA8_APP_LIBS)
    set(_epub_vendor ${RA8_REPO_ROOT}/apps/shared_libs/third_party/miniz/miniz.c)
    list(APPEND _ra8_lib_extra ${_epub_vendor})
    list(APPEND _ra8_lib_inc ${RA8_REPO_ROOT}/apps/shared_libs/third_party/miniz
         ${RA8_REPO_ROOT}/apps/shared_libs/epub/src
    )
  endif()

  if(("epub" IN_LIST _RA8_APP_LIBS) OR ("rabook_compile" IN_LIST _RA8_APP_LIBS))
    file(GLOB_RECURSE _xml CONFIGURE_DEPENDS ${RA8_REPO_ROOT}/apps/shared_libs/xml/src/*.c)
    _ra8_app_require_compilable_lib(
      xml
      "${RA8_REPO_ROOT}/apps/shared_libs/xml"
      "pulls in xml for LIBS epub/rabook_compile"
      "${_xml}"
      ""
    )
    list(APPEND _ra8_lib_extra ${_xml})
    list(APPEND _ra8_lib_inc ${RA8_REPO_ROOT}/apps/shared_libs/xml/inc)
  endif()

  # The RABOOK compiler includes the RBKC streaming container writer, whose
  # bounded compressor uses ra8_compress and miniz. Keep that dependency
  # transitive for every compiler consumer instead of requiring each app to
  # know which wire container the compiler can emit.
  set(_ra8_rabook_vendor "")
  if("rabook_compile" IN_LIST _RA8_APP_LIBS)
    list(APPEND _ra8_lib_inc ${RA8_REPO_ROOT}/apps/shared_libs/compress/inc
         ${RA8_REPO_ROOT}/apps/shared_libs/third_party/miniz
    )
    if(NOT "compress" IN_LIST _RA8_APP_LIBS)
      list(APPEND _ra8_lib_extra ${RA8_REPO_ROOT}/apps/shared_libs/compress/src/ra8_compress.c)
    endif()
    if((NOT "epub" IN_LIST _RA8_APP_LIBS) AND (NOT "miniz" IN_LIST _RA8_APP_LIBS))
      set(_ra8_rabook_vendor ${RA8_REPO_ROOT}/apps/shared_libs/third_party/miniz/miniz.c)
      list(APPEND _ra8_lib_extra ${_ra8_rabook_vendor})
    endif()
  endif()

  # jof transcodes JPEG/PNG sources into JOF tile atlases (#231).
  # Its PNG decoder inflates through the vendored miniz and its tile codec
  # reuses ra8_compress, so wire the miniz + compression includes when an app
  # pulls in jof. The miniz *implementation* TU comes from the
  # epub block above or the bare-miniz block below -- an app using
  # jof lists one of those alongside it. The single compress TU is
  # added directly when the app does not already list the compression seam.
  if("jof" IN_LIST _RA8_APP_LIBS)
    # JPEG is a pure-software Domain codec rather than a HAL backend. JOF owns
    # that dependency, so applications do not need to know which source image
    # codecs the atlas producer dispatches internally.
    if(NOT "ra8_jpeg" IN_LIST _RA8_APP_LIBS)
      file(GLOB_RECURSE _jof_jpeg CONFIGURE_DEPENDS ${RA8_REPO_ROOT}/libs/ra8_jpeg/src/*.c)
      _ra8_app_require_compilable_lib(
        ra8_jpeg
        "${RA8_REPO_ROOT}/libs/ra8_jpeg"
        "pulls in ra8_jpeg for LIBS jof"
        "${_jof_jpeg}"
        ""
      )
      list(APPEND _ra8_lib_extra ${_jof_jpeg})
    endif()
    list(
      APPEND
      _ra8_lib_inc
      ${RA8_REPO_ROOT}/apps/shared_libs/third_party/miniz
      ${RA8_REPO_ROOT}/apps/shared_libs/compress/inc
      ${RA8_REPO_ROOT}/libs/ra8_jpeg/inc
      ${RA8_REPO_ROOT}/libs/ra8_io/inc
    )
    if(NOT "compress" IN_LIST _RA8_APP_LIBS)
      list(APPEND _ra8_lib_extra ${RA8_REPO_ROOT}/apps/shared_libs/compress/src/ra8_compress.c)
    endif()
    # #290 normalize-on-import: the producer normalises WebP manifest images
    # to JOF too, so it calls the ra8_webp facade (the WebP arm lives in
    # jof_produce_webp.c: jof_priv_webp_transcode). Compile the
    # facade sources here when the app did
    # not already list webp explicitly (the LIBS loop globs them then).
    # The vendored libwebp decoder itself is wired by the shared block below.
    list(APPEND _ra8_lib_inc ${RA8_REPO_ROOT}/apps/shared_libs/webp/inc)
    if(NOT "webp" IN_LIST _RA8_APP_LIBS)
      file(GLOB_RECURSE _jof_webp_facade CONFIGURE_DEPENDS
           ${RA8_REPO_ROOT}/apps/shared_libs/webp/src/*.c
      )
      _ra8_app_require_compilable_lib(
        webp
        "${RA8_REPO_ROOT}/apps/shared_libs/webp"
        "pulls in the webp facade for LIBS jof"
        "${_jof_webp_facade}"
        ""
      )
      list(APPEND _ra8_lib_extra ${_jof_webp_facade})
    endif()
  endif()

  if("rabook_compile" IN_LIST _RA8_APP_LIBS)
    list(APPEND _ra8_lib_inc ${RA8_REPO_ROOT}/apps/shared_libs/webp/inc)
    if((NOT "webp" IN_LIST _RA8_APP_LIBS) AND (NOT "jof" IN_LIST _RA8_APP_LIBS))
      file(GLOB_RECURSE _ra8_rabook_webp_facade CONFIGURE_DEPENDS
           ${RA8_REPO_ROOT}/apps/shared_libs/webp/src/*.c
      )
      _ra8_app_require_compilable_lib(
        webp
        "${RA8_REPO_ROOT}/apps/shared_libs/webp"
        "pulls in the webp facade for LIBS rabook_compile"
        "${_ra8_rabook_webp_facade}"
        ""
      )
      list(APPEND _ra8_lib_extra ${_ra8_rabook_webp_facade})
    endif()
  endif()

  # #637 inline small-image WebP: reflow's ra8_img_decode_blit / ra8_img_probe_size
  # dispatch a RIFF/WEBP buffer to the ra8_webp facade, because stb_image has no
  # WebP decoder and an inline EPUB illustration would otherwise render as
  # nothing. So reflow now pulls the facade the same way jof and rabook_compile
  # do -- and only when no earlier block already added it, so the sources are
  # never double-added.
  if("reflow" IN_LIST _RA8_APP_LIBS)
    list(APPEND _ra8_lib_inc ${RA8_REPO_ROOT}/apps/shared_libs/webp/inc)
    if((NOT "webp" IN_LIST _RA8_APP_LIBS)
       AND (NOT "jof" IN_LIST _RA8_APP_LIBS)
       AND (NOT "rabook_compile" IN_LIST _RA8_APP_LIBS)
    )
      file(GLOB_RECURSE _ra8_reflow_webp_facade CONFIGURE_DEPENDS
           ${RA8_REPO_ROOT}/apps/shared_libs/webp/src/*.c
      )
      _ra8_app_require_compilable_lib(
        webp
        "${RA8_REPO_ROOT}/apps/shared_libs/webp"
        "pulls in the webp facade for LIBS reflow"
        "${_ra8_reflow_webp_facade}"
        ""
      )
      list(APPEND _ra8_lib_extra ${_ra8_reflow_webp_facade})
    endif()
  endif()

  # The webp app library decodes WebP (VP8 / VP8L) through the vendored decoder
  # (apps/shared_libs/third_party/libwebp). The four-part recipe -- which TUs, which
  # include root, -DRA8_WEBP_USE_ARENA, the SOUP warning flags -- lives in
  # cmake/ra8_webp_vendor.cmake and is NOT restated here: open-coding it is
  # what left the recipe unreachable from a standalone host tool, which is why
  # tools/rabook_imagepack and apps/host/mdl each faked jof_priv_webp_transcode()
  # rather than compile the decoder that was already in the tree.
  #
  # Its ra8_webp facade/arena are globbed by the LIBS loop above (or by
  # the jof block); only the vendored TUs + include root are wired
  # here. Wired whenever webp is requested directly OR pulled in
  # transitively by jof (#290), rabook_compile, or reflow (#637 inline
  # small-image WebP), and only once so those paths never double-add the
  # libwebp sources.
  set(_ra8_webp_vendor "")
  if(("webp" IN_LIST _RA8_APP_LIBS)
     OR ("jof" IN_LIST _RA8_APP_LIBS)
     OR ("rabook_compile" IN_LIST _RA8_APP_LIBS)
     OR ("reflow" IN_LIST _RA8_APP_LIBS)
  )
    include(${RA8_REPO_ROOT}/cmake/ra8_webp_vendor.cmake)
    ra8_webp_vendor_sources(_ra8_webp_vendor ${RA8_REPO_ROOT})
    list(APPEND _ra8_lib_extra ${_ra8_webp_vendor})
    list(APPEND _ra8_lib_inc ${RA8_REPO_ROOT}/apps/shared_libs/third_party/libwebp)
    # ra8_webp_imgdec.c binds the facade as an imgdec backend (#768) and reads
    # the container's declared geometry through the shared probe, which sniffs
    # first, so those two TUs and the header travel with this block. The reflow
    # / rabook_compile / comic blocks may already have added the same TUs, so
    # each is appended only when absent -- a duplicate source is an error under
    # some generators and a duplicate symbol under all of them.
    # One archive, and _ra8_lib_zig is de-duplicated where it is consumed
    # (_ra8_app_link_zig_libraries), so the blocks above may already have
    # named it.
    list(APPEND _ra8_lib_zig "ra8_imgdec|${RA8_REPO_ROOT}/libs/ra8_imgdec")
    if(NOT ${RA8_REPO_ROOT}/libs/ra8_imgdec/inc IN_LIST _ra8_lib_inc)
      list(APPEND _ra8_lib_inc ${RA8_REPO_ROOT}/libs/ra8_imgdec/inc)
    endif()
  endif()

  # unarch decodes wrapped / container archive streams (tar for .cbt,
  # gzip, XZ) under the unified decompression-limits policy. Its XZ leg
  # drives the vendored xz-embedded decoder (SOUP), whose allocator and mode
  # selection come from the first-party porting header
  # apps/shared_libs/unarch/inc/xz_config.h (zero-heap pool, XZ_PREALLOC only).
  # Wired whenever unarch is requested directly OR pulled in
  # transitively by comic (the CBT / wrapped-open backends), and only
  # once. The gzip leg reuses the miniz DEFLATE core supplied by the
  # epub block above or the bare-miniz block below.
  set(_ra8_xz_vendor "")
  if(("unarch" IN_LIST _RA8_APP_LIBS) OR ("comic" IN_LIST _RA8_APP_LIBS))
    set(_ra8_xz_vendor
        ${RA8_REPO_ROOT}/apps/shared_libs/third_party/xz_embedded/xz_crc32.c
        ${RA8_REPO_ROOT}/apps/shared_libs/third_party/xz_embedded/xz_crc64.c
        ${RA8_REPO_ROOT}/apps/shared_libs/third_party/xz_embedded/xz_dec_lzma2.c
        ${RA8_REPO_ROOT}/apps/shared_libs/third_party/xz_embedded/xz_dec_stream.c
    )
    list(APPEND _ra8_lib_extra ${_ra8_xz_vendor})
    list(
      APPEND
      _ra8_lib_inc
      ${RA8_REPO_ROOT}/apps/shared_libs/third_party/xz_embedded
      ${RA8_REPO_ROOT}/apps/shared_libs/unarch/inc
      ${RA8_REPO_ROOT}/apps/shared_libs/third_party/miniz
    )
    # unarch_xz_pool.c forwards its bump arithmetic to the shared decoder
    # scratch, and comic_tiles.c reads a page's footprint through the shared
    # geometry probe, which sniffs first (#768), so those three TUs and the
    # header travel with this block. The reflow / rabook_compile blocks above
    # may already have added the same TUs, so each is appended only when
    # absent -- a duplicate source is an error under some generators and a
    # duplicate symbol under all of them.
    list(APPEND _ra8_lib_zig "ra8_imgdec|${RA8_REPO_ROOT}/libs/ra8_imgdec")
    if(NOT ${RA8_REPO_ROOT}/libs/ra8_imgdec/inc IN_LIST _ra8_lib_inc)
      list(APPEND _ra8_lib_inc ${RA8_REPO_ROOT}/libs/ra8_imgdec/inc)
    endif()
    if(NOT "unarch" IN_LIST _RA8_APP_LIBS)
      file(GLOB_RECURSE _unarch_srcs CONFIGURE_DEPENDS
           ${RA8_REPO_ROOT}/apps/shared_libs/unarch/src/*.c
      )
      _ra8_app_require_compilable_lib(
        unarch
        "${RA8_REPO_ROOT}/apps/shared_libs/unarch"
        "pulls in unarch transitively"
        "${_unarch_srcs}"
        ""
      )
      list(APPEND _ra8_lib_extra ${_unarch_srcs})
    endif()
  endif()

  # A bare "miniz" in LIBS pulls in just the vendored DEFLATE core, for apps
  # that inflate compressed blobs directly (e.g. book RBKC containers via the
  # heap-free tinfl_decompress) without epub's full ZIP + XML stack. Skipped
  # when epub is present, which already compiles miniz.c above.
  set(_ra8_miniz_vendor "")
  if(("miniz" IN_LIST _RA8_APP_LIBS) AND (NOT "epub" IN_LIST _RA8_APP_LIBS))
    set(_ra8_miniz_vendor ${RA8_REPO_ROOT}/apps/shared_libs/third_party/miniz/miniz.c)
    list(APPEND _ra8_lib_extra ${_ra8_miniz_vendor})
    list(APPEND _ra8_lib_inc ${RA8_REPO_ROOT}/apps/shared_libs/third_party/miniz)
  endif()

  # ra8_io_blockdev_vsource.c (globbed in by a bare "ra8_io" in LIBS) is the
  # sanctioned Ring-4 -> Ring-2 bridge that exposes a block device as an
  # ra8_vsource read callback for the issue #147 page cache. It includes
  # ra8_vsource.h from ra8_mem, so it only links -- and only adds ra8_mem/inc to
  # the include path -- when the app also declares "ra8_mem" in LIBS. Mirrors the
  # app-owned compression composition: a plain ra8_io consumer that never wires a
  # block device into the page cache pays nothing and needs no ra8_mem on its
  # include path. The header (ra8_io_blockdev_vsource.h) is likewise opt-in, kept
  # out of the ra8_io.h umbrella.
  if("ra8_mem" IN_LIST _RA8_APP_LIBS)
    list(APPEND _ra8_lib_inc ${RA8_REPO_ROOT}/libs/ra8_mem/inc)
  else()
    list(
      FILTER
      _ra8_lib_extra
      EXCLUDE
      REGEX
      "ra8_io/src/ra8_io_blockdev_vsource\\.c$"
    )
  endif()

  # A bare "ra8_io_bus" in LIBS pulls in just the ra8_io SPI/I2C bus facades
  # (ra8_io_spi_bus*.c, ra8_io_i2c_bus*.c) plus the libs/ra8_io/inc include
  # path, for apps that bind a device driver's bus seam (ra8_spi_bus_ops_t /
  # ra8_i2c_bus_ops_t) without the rest of the ra8_io fabric. The bus facade
  # TUs depend only on ra8_hal drivers, which every app already compiles, so
  # -- unlike the full "ra8_io" -- this needs no ra8_fs / ra8_sdmmc_spi /
  # ra8_usb_pal companions. Mirrors the bare-"miniz" pseudo-lib above; the
  # LIBS loop's libs/ra8_io_bus/src glob is harmlessly empty. Skipped when
  # the full "ra8_io" is present, which already compiles these TUs. A bare
  # "ra8_camera" gets the same treatment: the board camera adapter it needs
  # (below) is written against the same I2C facade, and camera_capture
  # declares only "ra8_camera ra8_ov5640".
  if((("ra8_io_bus" IN_LIST _RA8_APP_LIBS) OR ("ra8_camera" IN_LIST _RA8_APP_LIBS))
     AND (NOT "ra8_io" IN_LIST _RA8_APP_LIBS)
  )
    file(
      GLOB
      _ra8_io_bus_srcs
      CONFIGURE_DEPENDS
      ${RA8_REPO_ROOT}/libs/ra8_io/src/ra8_io_spi_bus*.c
      ${RA8_REPO_ROOT}/libs/ra8_io/src/ra8_io_i2c_bus*.c
    )
    list(APPEND _ra8_lib_extra ${_ra8_io_bus_srcs})
    list(APPEND _ra8_lib_inc ${RA8_REPO_ROOT}/libs/ra8_io/inc)
  endif()

  # The board glob above compiles EVERY BSP translation unit into EVERY app,
  # which works because the rest of the BSP depends only on libraries whose
  # include paths are unconditional (ra8_core, ra8_hal, ra8_net_pal,
  # ra8_usb_pal). Two BSP units do not, so each is dropped unless the app
  # declared the library it needs:
  #
  #   ..._console_stream.c  hands back an ra8_io_stream_t, so it needs
  #                         libs/ra8_io/inc AND the ra8_io_stream TUs at link
  #                         time -> the full "ra8_io". A bare "ra8_io_bus" is
  #                         NOT enough: it compiles the bus facades only.
  #   ..._camera.c          publishes the J35 SCCB adapter as an
  #                         ra8_i2c_bus_ops_t over ra8_io_i2c_bus_riic, so it
  #                         needs libs/ra8_io/inc and the I2C facade TUs ->
  #                         "ra8_io", "ra8_io_bus", OR "ra8_camera".
  #   ..._touch.c           binds the GT911 bus through the ra8_io I2C facade
  #                         -> "ra8_io" OR "ra8_io_bus", either of which
  #                         compiles ra8_io_i2c_bus*.c.
  #
  # Both headers are kept out of the ra8_board_ek_ra8d2.h umbrella too, so an
  # app that never asks pays neither an include path nor an object. Mirrors the
  # app-compress / ra8_io_blockdev_vsource gates above, which drop a TU whose
  # companion library is absent.
  #
  # BOTH lists, and that is not belt-and-braces. An app may also name
  # "ra8_board_ek_ra8d2" in LIBS -- glcdc_render and widget_kit_demo do -- and
  # the LIBS loop then globs libs/<name>/src/*.c into _ra8_lib_extra, a SECOND
  # copy of every BSP unit that a filter over _ra8_lib_board alone does not
  # reach. Filtering one list left those apps compiling an opt-in TU they had
  # not opted into, and the failure was a missing header rather than anything
  # that named the gate.
  if(NOT "ra8_io" IN_LIST _RA8_APP_LIBS)
    foreach(_ra8_board_list _ra8_lib_board _ra8_lib_extra)
      list(
        FILTER
        ${_ra8_board_list}
        EXCLUDE
        REGEX
        "ra8_board_[a-z0-9_]+_console_stream\\.c$"
      )
    endforeach()
  endif()
  if(NOT (("ra8_io" IN_LIST _RA8_APP_LIBS) OR ("ra8_io_bus" IN_LIST _RA8_APP_LIBS)))
    foreach(_ra8_board_list _ra8_lib_board _ra8_lib_extra)
      list(
        FILTER
        ${_ra8_board_list}
        EXCLUDE
        REGEX
        "ra8_board_[a-z0-9_]+_touch\\.c$"
      )
    endforeach()
  endif()
  if(NOT
     (("ra8_io" IN_LIST _RA8_APP_LIBS)
      OR ("ra8_io_bus" IN_LIST _RA8_APP_LIBS)
      OR ("ra8_camera" IN_LIST _RA8_APP_LIBS))
  )
    foreach(_ra8_board_list _ra8_lib_board _ra8_lib_extra)
      list(
        FILTER
        ${_ra8_board_list}
        EXCLUDE
        REGEX
        "ra8_board_[a-z0-9_]+_camera\\.c$"
      )
    endforeach()
  endif()

  # The vendored SOUP decoders (miniz DEFLATE, stb image/truetype) type-pun
  # through byte buffers, which violates C strict-aliasing. GCC's aliasing
  # optimizations at -Og/-O2 then miscompile them: arm-none-eabi-gcc 13.3
  # corrupts miniz's inflate so EPUB/RBKC extraction fails (epub_open ->
  # "FAIL open") on the official toolchain, while older toolchains happen not
  # to trip it. Build just these third_party TUs with -fno-strict-aliasing --
  # the upstream-sanctioned flag for this code -- so the -Og default is
  # correct on every toolchain. First-party sources stay strict-aliasing clean.
  set(_ra8_soup_tu
      ${_epub_vendor}
      ${_ra8_rabook_vendor}
      ${_ra8_miniz_vendor}
      ${_ra8_stb_impl}
      ${_ra8_stb_img_impl}
  )
  if(_ra8_soup_tu)
    set_source_files_properties(${_ra8_soup_tu} PROPERTIES COMPILE_OPTIONS -fno-strict-aliasing)
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
  # re-confirmed on 2026-08-25 by removing it ON ITS OWN, with the rest of the
  # list still applied, across the 53 SOUP compiles in the unified RA8D2
  # configure, under the PINNED cross toolchain arm-none-eabi-gcc 13.3.1
  # (/opt/arm-gnu-toolchain-13.3, cmake/toolchain-ra8d2.cmake) at -O0, -Og and
  # -Os. None is a memory-safety class. That re-measurement also deleted
  # -Wno-float-conversion, which had never fired: gcc's -Wconversion enables
  # -Wfloat-conversion for C, so -Wno-conversion already covered it. The
  # toolchain this block previously named, "14.3", is not a pin this tree has
  # ever had.
  set(_ra8_soup_wno_common
      -Wno-cast-qual # miniz.c casts away const on its byte buffers
      -Wno-cast-align # stb_image.h casts uint8_t* to a wider stbi__uint16*
      -Wno-double-promotion # stb_truetype.h float -> double in its math hooks
      -Wno-unused-parameter # stb_truetype.h stbtt_GetGlyphBox(info) formals
      -Wno-type-limits # miniz.c range-limited comparison always false
      -Wno-duplicated-branches # stb_image.h identical if/else arms
      -Wno-missing-declarations # stb_image.h globals with no prior declaration
      -Wno-conversion # stb/miniz narrowing; also covers -Wfloat-conversion
      -Wno-sign-conversion # stb_image.h int -> size_t index math
      # stb_image's GIF path (stbi__gif_load / stbi__load_gif_main) puts ~34 KiB
      # of frame buffers on the stack -- far over the per-app -Wstack-usage=N
      # budget, but an inherent property of the vendored decoder we cannot shrink
      # without editing SOUP. The blanket -w hid this too; suppress only the
      # warning here. -fstack-usage still emits the .su data, so the real
      # project-wide stack-bound proof (scripts/checks/stack_usage_check.py over
      # the ARM .su files) still sees these frames. Measured: miniz.c's
      # tinfl_decompress_mem_to_heap frame is 8448 bytes against the 2200-byte
      # per-app budget.
      -Wno-stack-usage # miniz tinfl_decompress_mem_to_heap has an 8448-byte SOUP frame
  )
  set(_ra8_soup_wno_c -Wno-bad-function-cast # stb_truetype casts a call result
                      -Wno-missing-prototypes
  ) # stb globals with no prior prototype

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

  # THREADX_HEAP <region>: compose the board map instead of forking it (#761).
  #
  # ThreadX apps need exactly one symbol the board map does not define --
  # g_ra8_threadx_unused_memory_start, the origin of the region tx_application_
  # define() carves its pools from. Before this option the only way to add it
  # was a per-app linker_script.ld, so 42 apps each carried a private copy of
  # the whole 340-line board map to gain one PROVIDE line. Those copies then
  # missed every later platform change: the NOINIT crash-log carve-out landed
  # in the board map and none of the 42 picked it up.
  #
  # Instead of copying the map, generate a two-line fragment that INCLUDEs it
  # by absolute path and adds the PROVIDE. The board map stays the single
  # source of the region layout, so a future change to it reaches these apps
  # the same day it lands.
  #
  # The INCLUDE path must be absolute: a linker script named by an absolute -T
  # resolves a relative INCLUDE against the linker's working directory, not
  # against the including script, so a bare name is not found from the build
  # tree. LINK_DEPENDS carries both files, so editing either one relinks.
  #
  # CPU1_IMAGE composes the same way (#742). A dual-core app needs one output
  # section and two symbols the board map does not carry, and the only way to
  # get them used to be a private linker_script.ld -- so nine apps forked the
  # whole 340-line map for 9 lines of content, and then missed the NOINIT
  # crash-log carve-out exactly as the ThreadX forks did.
  #
  # .cpu1_image is pinned by absolute address rather than by splitting MRAM
  # into MRAM + MRAM_CPU1. An INCLUDEd script's MEMORY block cannot be amended
  # by the including one, so a region split is not composable at all; an
  # absolute placement is, and it says the same thing. The region accounting
  # that the split used to provide is replaced by the explicit ASSERT below.
  # It measures the load end of .dtcm_data, the last section the board map
  # places AT > MRAM, so it catches an M85 image that grew into the CPU1
  # window. Without it that overlap would be silent.
  #
  # SRAM_TEXT <file.c>... is the third case (#742), and it does NOT compose by
  # appending. Flash-writing code cannot execute from the MRAM it is erasing,
  # so the DFU apps run ra8_flash.c and ra8_dfu_program.c from an SRAM-resident
  # .sram_text section loaded from MRAM at boot. Appending that section after
  # the INCLUDE links and produces an EMPTY section: ld assigns each input
  # section to the first output section in script order that matches, and the
  # board map's .text catch-all has already claimed those objects. The link is
  # clean and the flash loop silently runs from MRAM, which is why the three
  # forks spliced an EXCLUDE_FILE list into their private copy of .text.
  #
  # So the board map carries an injection point instead: an INCLUDE of
  # ra8_app_pre_text.ld sitting between .vectors and .text. This macro always
  # writes that file into the app's build dir and puts the dir on the linker
  # search path, so a bare name resolves. Sitting ahead of .text, the
  # generated .sram_text claims the named objects by the same first-match rule
  # and no EXCLUDE_FILE is needed anywhere.
  #
  # The option names sources rather than taking a boolean because which code
  # must run from SRAM is an app decision, not a board one. Both .obj and .o
  # spellings are emitted: the object suffix follows the generator, and naming
  # only one would silently place nothing under the other. The bare
  # *(.sram_text) wildcards come first so the
  # __attribute__((section(".sram_text"))) route that ra8_flash.h documents
  # lands in the same section instead of being placed as an orphan.
  # MRAM_LENGTH <size>: the bootloader-bank apps link against a 128K MRAM bank
  # rather than the full 1024K. A MEMORY block in an INCLUDEd script cannot be
  # amended by the including one, so before this there was no way to change one
  # number except to copy the whole map -- which is exactly what dfu_copy_to_run
  # and secure_boot_hil did, at 344 lines each. The board map now reads its MRAM
  # length through DEFINED(__ra8_app_mram_length), and this file supplies the
  # override ahead of the MEMORY block. An empty file leaves the canonical map
  # untouched, so every other app is unaffected.
  set(_ra8_ld_pre_memory "${CMAKE_CURRENT_BINARY_DIR}/ra8_app_pre_memory.ld")
  if(_RA8_APP_MRAM_LENGTH)
    file(WRITE "${_ra8_ld_pre_memory}"
         "/* Generated by ra8_add_app(MRAM_LENGTH ${_RA8_APP_MRAM_LENGTH}). Do not edit. */\n"
         "__ra8_app_mram_length = ${_RA8_APP_MRAM_LENGTH};\n"
    )
  else()
    file(WRITE "${_ra8_ld_pre_memory}"
         "/* Generated by ra8_add_app(). Board memory map used as written. */\n"
    )
  endif()

  set(_ra8_ld_pre_text "${CMAKE_CURRENT_BINARY_DIR}/ra8_app_pre_text.ld")
  if(_RA8_APP_SRAM_TEXT)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/linker_script.ld")
      message(
        FATAL_ERROR
          "ra8_add_app(): ${_RA8_APP_NAME} passes SRAM_TEXT but also has its "
          "own linker_script.ld, which carries no injection point. An app "
          "with a local map already has full control: put the .sram_text "
          "section in that script, or delete it to compose the board map."
      )
    endif()
    set(_ra8_sram_text_body "")
    foreach(_ra8_src IN LISTS _RA8_APP_SRAM_TEXT)
      string(APPEND _ra8_sram_text_body "        *${_ra8_src}.obj(.text .text.*)\n"
             "        *${_ra8_src}.o(.text .text.*)\n"
      )
    endforeach()
    list(JOIN _RA8_APP_SRAM_TEXT " " _ra8_sram_text_why)
    file(
      WRITE "${_ra8_ld_pre_text}"
      "/* Generated by ra8_add_app(SRAM_TEXT ${_ra8_sram_text_why}). Do not edit. */\n"
      "    .sram_text : ALIGN(4)\n"
      "    {\n"
      "        g_ra8_ls_ssram_text = .;\n"
      "        *(.sram_text)\n"
      "        *(.sram_text.*)\n"
      "${_ra8_sram_text_body}"
      "        g_ra8_ls_esram_text = .;\n"
      "    } > SRAM AT > MRAM\n"
      "    g_ra8_ls_sram_text_load = LOADADDR(.sram_text);\n"
    )
  else()
    file(WRITE "${_ra8_ld_pre_text}"
         "/* Generated by ra8_add_app(). Nothing to inject ahead of .text. */\n"
    )
  endif()

  set(_ra8_ld_lines "")
  set(_ra8_ld_why "")
  if(_RA8_APP_THREADX_HEAP)
    list(APPEND _ra8_ld_why "THREADX_HEAP ${_RA8_APP_THREADX_HEAP}")
    string(APPEND _ra8_ld_lines
           "PROVIDE(g_ra8_threadx_unused_memory_start = ORIGIN(${_RA8_APP_THREADX_HEAP}));\n"
    )
  endif()
  if(_RA8_APP_CPU1_IMAGE)
    list(APPEND _ra8_ld_why "CPU1_IMAGE")
    include(${_ra8_board_dir}/ld/cpu1_memory_map.cmake)
    # Read from a tracked template rather than built as a string here, so the
    # two g_ra8_ls_cpu1_* symbols are declared in a file the linker-script
    # checkers scan. See cpu1_image.ld.in for why the section is addressed.
    file(READ ${_ra8_board_dir}/ld/cpu1_image.ld.in _ra8_cpu1_tpl)
    string(CONFIGURE "${_ra8_cpu1_tpl}" _ra8_cpu1_frag @ONLY)
    string(APPEND _ra8_ld_lines "${_ra8_cpu1_frag}")
  endif()
  if(_RA8_APP_NS_INLINE_IMAGE)
    list(APPEND _ra8_ld_why "NS_INLINE_IMAGE")
    include(${_ra8_board_dir}/ld/ns_inline_memory_map.cmake)
    # Placed at absolute addresses rather than into MEMORY regions, exactly as
    # CPU1_IMAGE is: the board map's MEMORY block is INCLUDEd, and an INCLUDEd
    # MEMORY cannot be amended by a later fragment. The board map does carry
    # NS_MRAM / NS_SRAM placeholders, but at 512K / 640K they are the wrong
    # size for a dual-core app -- see ns_inline_memory_map.cmake, where the
    # 576K SRAM bound is a silicon fix and not a tidy-up.
    #
    # EVERY section here carries an explicit address, and the two that follow
    # .ns_vectors derive theirs with ADDR()+SIZEOF() rather than riding the
    # location counter. Both cheaper-looking spellings were tried and both
    # mislaid the image, measured on cpu1_pingpong_ipc:
    #
    #   * address on .ns_vectors only, .ns_text/.ns_rodata bare -- ld places
    #     the addressed section, then resumes the REGION-LESS counter it was
    #     already carrying, so .ns_text landed at 0x02001570, inside Secure
    #     MRAM just past .bss's load address, while .ns_vectors sat correctly
    #     at 0x02080000.
    #   * `. = <origin>;` ahead of a bare .ns_vectors -- a dot assignment in a
    #     SECTIONS block appended after the board map does not carry, and the
    #     whole NS group slid to 0x02001570 with .ns_bss at 0.
    #
    # An appended SECTIONS block gets no usable counter from the board map, so
    # anything that must land at a fixed address has to say so itself. This is
    # also why CPU1_IMAGE above spells .cpu1_image's address out.
    string(
      APPEND
      _ra8_ld_lines
      "SECTIONS\n"
      "{\n"
      "    .ns_vectors ${RA8_NS_INLINE_MRAM_ORIGIN} : ALIGN(8)\n"
      "    {\n"
      "        KEEP(*(.ns_vectors))\n"
      "        KEEP(*(.ns_vectors.*))\n"
      "    }\n"
      "    .ns_text ADDR(.ns_vectors) + SIZEOF(.ns_vectors) : ALIGN(4)\n"
      "    {\n"
      "        *(.ns_text)\n"
      "        *(.ns_text.*)\n"
      "    }\n"
      "    .ns_rodata ADDR(.ns_text) + SIZEOF(.ns_text) : ALIGN(4)\n"
      "    {\n"
      "        *(.ns_rodata)\n"
      "        *(.ns_rodata.*)\n"
      "    }\n"
      "    .ns_bss ${RA8_NS_INLINE_SRAM_ORIGIN} (NOLOAD) : ALIGN(4)\n"
      "    {\n"
      "        g_ra8_ls_ns_bss_start = .;\n"
      "        *(.ns_bss)\n"
      "        *(.ns_bss.*)\n"
      "        g_ra8_ls_ns_bss_end = .;\n"
      "    }\n"
      "}\n"
      # Slot 0 of the NS vector table is the initial MSP_NS; BLXNS in
      # ra8_tz_secure_boot_jump_ns issues `msr msp_ns` with this value.
      "g_ra8_ls_ns_stack_top = ${RA8_NS_INLINE_SRAM_ORIGIN} + ${RA8_NS_INLINE_SRAM_LENGTH};\n"
      "ASSERT(ADDR(.ns_rodata) + SIZEOF(.ns_rodata)\n"
      "       <= ${RA8_NS_INLINE_MRAM_ORIGIN} + ${RA8_NS_INLINE_MRAM_LENGTH},\n"
      "       \"FATAL: the NS image overran its window\")\n"
      "ASSERT(g_ra8_ls_ns_bss_end <= g_ra8_ls_ns_stack_top,\n"
      "       \"FATAL: NS .bss collided with the NS stack\")\n"
    )
  endif()
  # linker_append.ld: the escape hatch for a section only one app needs, so that
  # wanting one does not cost a fork of the whole map. dfu_copy_to_run pins two
  # probe words at a fixed SRAM address for the DFU host to read; that is six
  # lines of app-specific placement with no plausible second user, and inventing
  # a named option for it would be worse than letting the app say it directly.
  # Sections here are appended after the board map, so this is the right place
  # only for placement nothing else competes for: anything that has to claim
  # input sections ahead of .text belongs in the pre-text file above, where
  # first-match ordering works in its favour.
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/linker_append.ld")
    list(APPEND _ra8_ld_why "linker_append.ld")
    string(APPEND _ra8_ld_lines "INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/linker_append.ld\n")
    list(APPEND _ra8_ld_extra_deps "${CMAKE_CURRENT_SOURCE_DIR}/linker_append.ld")
  endif()
  if(_ra8_ld_lines)
    list(JOIN _ra8_ld_why " + " _ra8_ld_why)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/linker_script.ld")
      message(
        FATAL_ERROR
          "ra8_add_app(): ${_RA8_APP_NAME} passes ${_ra8_ld_why} but also has "
          "its own linker_script.ld. An app with a local map already has full "
          "control: add those lines to that script, or delete it to compose " "the board map."
      )
    endif()
    set(_ra8_ld_fragment "${CMAKE_CURRENT_BINARY_DIR}/${_RA8_APP_NAME}_composed.ld")
    file(WRITE "${_ra8_ld_fragment}"
         "/* Generated by ra8_add_app(${_ra8_ld_why}). Do not edit. */\n"
         "INCLUDE ${_ra8_linker}\n" "${_ra8_ld_lines}"
    )
    set(_ra8_ld_base ${_ra8_linker})
    set(_ra8_linker ${_ra8_ld_fragment})
  endif()
  set(_ra8_elf ${_RA8_APP_NAME}.elf)
endmacro()
