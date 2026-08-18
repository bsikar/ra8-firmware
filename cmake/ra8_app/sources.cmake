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
# cmake-lint: disable=R0912,R0915
#
# The branch and statement ceilings are waived here for the same reason
# ra8_add_app() waives them, and the reason survives the move: these branches
# ARE the library matrix -- one arm per library an app can name in LIBS, plus
# the vendored SOUP each of those drags in. Splitting further would not remove
# a branch, it would scatter one table across several files, and "which
# sources does LIBS ra8_reflow pull in" would stop being answerable by reading
# one list. The waiver is per-file; the global ceilings in .cmake-format.yaml
# stay at cmakelang defaults so no other listfile inherits it.
macro(_ra8_app_collect_sources)
  # ---- sources: per-app main, shared-or-local boot ----------------------
  set(_ra8_src ${CMAKE_CURRENT_SOURCE_DIR}/main.c)
  foreach(
    _ra8_boot
    vector_table.c
    system_init.c
    secure_exception.c
    nmi_exception.c
    trustzone_init.c
  )
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

  file(GLOB_RECURSE _ra8_lib_core CONFIGURE_DEPENDS ${RA8_REPO_ROOT}/libs/ra8_core/src/*.c)
  file(GLOB_RECURSE _ra8_lib_hal CONFIGURE_DEPENDS ${RA8_REPO_ROOT}/libs/ra8_hal/src/*.c)
  file(GLOB_RECURSE _ra8_lib_net_pal CONFIGURE_DEPENDS ${RA8_REPO_ROOT}/libs/ra8_net_pal/src/*.c)
  file(GLOB_RECURSE _ra8_lib_usb_pal CONFIGURE_DEPENDS ${RA8_REPO_ROOT}/libs/ra8_usb_pal/src/*.c)
  file(GLOB_RECURSE _ra8_lib_board CONFIGURE_DEPENDS ${_ra8_board_dir}/src/*.c)
  file(GLOB_RECURSE _ra8_secure_app CONFIGURE_DEPENDS ${RA8_REPO_ROOT}/libs/ra8_secure_app/src/*.c)
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

  # Extra first-party libraries (plain + off-target).
  set(_ra8_lib_extra "")
  set(_ra8_lib_extra_off_target "")
  set(_ra8_lib_inc "")
  foreach(_ra8_lib ${_RA8_APP_LIBS})
    file(GLOB_RECURSE _ra8_lib_one CONFIGURE_DEPENDS ${RA8_REPO_ROOT}/libs/${_ra8_lib}/src/*.c)
    list(APPEND _ra8_lib_extra ${_ra8_lib_one})
    list(APPEND _ra8_lib_inc ${RA8_REPO_ROOT}/libs/${_ra8_lib}/inc)
  endforeach()
  # Generated protobuf-c output contains casts required by its runtime ABI.
  # Keep warnings enabled for the handwritten client and service while
  # treating this one generated translation unit like the vendored codec.
  if("ra8_c6link" IN_LIST _RA8_APP_LIBS)
    # The media RPC carries the canonical ra8_mdl_format_t in its public
    # request contract. Consumers need the declaration even when they use
    # c6link only for Wi-Fi and do not otherwise compile ra8_mdl sources.
    list(APPEND _ra8_lib_inc ${RA8_REPO_ROOT}/libs/ra8_mdl/inc)
    set_source_files_properties(
      ${RA8_REPO_ROOT}/libs/ra8_c6link/src/ra8_media_download.pb-c.c PROPERTIES COMPILE_OPTIONS
                                                                                "-w"
    )
  endif()
  foreach(_ra8_lib ${_RA8_APP_OFF_TARGET_LIBS})
    file(GLOB_RECURSE _ra8_lib_one CONFIGURE_DEPENDS ${RA8_REPO_ROOT}/libs/${_ra8_lib}/src/*.c)
    list(APPEND _ra8_lib_extra_off_target ${_ra8_lib_one})
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
    list(APPEND _ra8_lib_inc ${RA8_REPO_ROOT}/libs/third_party/stb
         ${RA8_REPO_ROOT}/libs/ra8_reflow/src
    )
  elseif("ra8_rabook_compile" IN_LIST _RA8_APP_LIBS)
    set(_ra8_stb_img_impl ${RA8_REPO_ROOT}/libs/third_party/stb/stb_image_impl.c)
    list(APPEND _ra8_lib_extra ${_ra8_stb_img_impl}
         ${RA8_REPO_ROOT}/libs/ra8_reflow/src/ra8_img_arena.c
    )
    list(
      APPEND
      _ra8_lib_inc
      ${RA8_REPO_ROOT}/libs/third_party/stb
      ${RA8_REPO_ROOT}/libs/ra8_reflow/inc
      ${RA8_REPO_ROOT}/libs/ra8_reflow/src
    )
  endif()

  # ra8_reflow's glyph rasteriser (ra8_reflow_render.c, #164) caches glyph bitmaps
  # through the Layer-3 ra8_glyph_atlas, and ra8_book's paged accessor
  # (ra8_book_paged.c / ra8_book_xhtml.c, #163) reads books through the ra8_vmem
  # page cache -- both in libs/ra8_mem. ra8_mem depends only on ra8_core, so wire
  # it in automatically for ra8_reflow / ra8_book consumers (mirroring the stb
  # special-case above), unless the app already lists ra8_mem in LIBS -- in which
  # case the loop above already globbed it.
  if((("ra8_reflow" IN_LIST _RA8_APP_LIBS) OR ("ra8_book" IN_LIST _RA8_APP_LIBS))
     AND (NOT "ra8_mem" IN_LIST _RA8_APP_LIBS)
  )
    file(GLOB_RECURSE _ra8_lib_mem CONFIGURE_DEPENDS ${RA8_REPO_ROOT}/libs/ra8_mem/src/*.c)
    list(APPEND _ra8_lib_extra ${_ra8_lib_mem})
    list(APPEND _ra8_lib_inc ${RA8_REPO_ROOT}/libs/ra8_mem/inc)
  endif()

  # ra8_camera always compiles its software-JPEG codec backend
  # (ra8_camera_codec_jpeg_sw.c), which calls ra8_jpeg_sw_encode(). Software
  # JPEG is a pure-software Domain codec and moved out of ra8_hal into its own
  # libs/ra8_jpeg, so it is no longer swept in by the always-globbed HAL. Wire
  # it transitively here for the same reason the ra8_jof block below does:
  # which image codec the camera driver dispatches to internally is the
  # driver's business, not the application's.
  if("ra8_camera" IN_LIST _RA8_APP_LIBS)
    if(NOT "ra8_jpeg" IN_LIST _RA8_APP_LIBS)
      file(GLOB_RECURSE _ra8_camera_jpeg CONFIGURE_DEPENDS ${RA8_REPO_ROOT}/libs/ra8_jpeg/src/*.c)
      list(APPEND _ra8_lib_extra ${_ra8_camera_jpeg})
    endif()
    list(APPEND _ra8_lib_inc ${RA8_REPO_ROOT}/libs/ra8_jpeg/inc)
  endif()

  # ra8_epub parses ZIP through vendored miniz and XML through the first-party
  # bounded ra8_xml pull reader. Only miniz is SOUP; no C++ parser is linked.
  set(_ra8_epub_vendor "")
  if("ra8_epub" IN_LIST _RA8_APP_LIBS)
    set(_ra8_epub_vendor ${RA8_REPO_ROOT}/libs/third_party/miniz/miniz.c)
    list(APPEND _ra8_lib_extra ${_ra8_epub_vendor})
    list(APPEND _ra8_lib_inc ${RA8_REPO_ROOT}/libs/third_party/miniz
         ${RA8_REPO_ROOT}/libs/ra8_epub/src
    )
  endif()

  if(("ra8_epub" IN_LIST _RA8_APP_LIBS) OR ("ra8_rabook_compile" IN_LIST _RA8_APP_LIBS))
    file(GLOB_RECURSE _ra8_xml CONFIGURE_DEPENDS ${RA8_REPO_ROOT}/libs/ra8_xml/src/*.c)
    list(APPEND _ra8_lib_extra ${_ra8_xml})
    list(APPEND _ra8_lib_inc ${RA8_REPO_ROOT}/libs/ra8_xml/inc)
  endif()

  # The RABOOK compiler includes the RBKC streaming container writer, whose
  # bounded compressor uses ra8_io_compress and miniz. Keep that dependency
  # transitive for every compiler consumer instead of requiring each app to
  # know which wire container the compiler can emit.
  set(_ra8_rabook_vendor "")
  if("ra8_rabook_compile" IN_LIST _RA8_APP_LIBS)
    list(APPEND _ra8_lib_inc ${RA8_REPO_ROOT}/libs/ra8_io/inc
         ${RA8_REPO_ROOT}/libs/third_party/miniz
    )
    if(NOT "ra8_io" IN_LIST _RA8_APP_LIBS)
      list(APPEND _ra8_lib_extra ${RA8_REPO_ROOT}/libs/ra8_io/src/ra8_io_compress.c)
    endif()
    if((NOT "ra8_epub" IN_LIST _RA8_APP_LIBS) AND (NOT "miniz" IN_LIST _RA8_APP_LIBS))
      set(_ra8_rabook_vendor ${RA8_REPO_ROOT}/libs/third_party/miniz/miniz.c)
      list(APPEND _ra8_lib_extra ${_ra8_rabook_vendor})
    endif()
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
    # JPEG is a pure-software Domain codec rather than a HAL backend. JOF owns
    # that dependency, so applications do not need to know which source image
    # codecs the atlas producer dispatches internally.
    if(NOT "ra8_jpeg" IN_LIST _RA8_APP_LIBS)
      file(GLOB_RECURSE _ra8_jof_jpeg CONFIGURE_DEPENDS ${RA8_REPO_ROOT}/libs/ra8_jpeg/src/*.c)
      list(APPEND _ra8_lib_extra ${_ra8_jof_jpeg})
    endif()
    list(
      APPEND
      _ra8_lib_inc
      ${RA8_REPO_ROOT}/libs/third_party/miniz
      ${RA8_REPO_ROOT}/libs/ra8_jpeg/inc
      ${RA8_REPO_ROOT}/libs/ra8_io/inc
    )
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
           ${RA8_REPO_ROOT}/libs/ra8_webp/src/*.c
      )
      list(APPEND _ra8_lib_extra ${_ra8_jof_webp_facade})
    endif()
  endif()

  if("ra8_rabook_compile" IN_LIST _RA8_APP_LIBS)
    list(APPEND _ra8_lib_inc ${RA8_REPO_ROOT}/libs/ra8_webp/inc)
    if((NOT "ra8_webp" IN_LIST _RA8_APP_LIBS) AND (NOT "ra8_jof" IN_LIST _RA8_APP_LIBS))
      file(GLOB_RECURSE _ra8_rabook_webp_facade CONFIGURE_DEPENDS
           ${RA8_REPO_ROOT}/libs/ra8_webp/src/*.c
      )
      list(APPEND _ra8_lib_extra ${_ra8_rabook_webp_facade})
    endif()
  endif()

  # ra8_webp decodes WebP (VP8 / VP8L) through the vendored libwebp decoder
  # (libs/third_party/libwebp). The four-part recipe -- which TUs, which
  # include root, -DRA8_WEBP_USE_ARENA, the SOUP warning flags -- lives in
  # cmake/ra8_webp_vendor.cmake and is NOT restated here: open-coding it is
  # what left the recipe unreachable from a standalone host tool, which is why
  # tools/rabook_imagepack and apps/stand_alone/media_dl each faked ra8_jof_priv_webp_transcode()
  # rather than compile the decoder that was already in the tree.
  #
  # ra8_webp's own .c facade/arena are globbed by the LIBS loop above (or by
  # the ra8_jof block); only the vendored TUs + include root are wired
  # here. Wired whenever ra8_webp is requested directly OR pulled in
  # transitively by ra8_jof (#290), and only once so the two paths never
  # double-add the libwebp sources.
  set(_ra8_webp_vendor "")
  if(("ra8_webp" IN_LIST _RA8_APP_LIBS)
     OR ("ra8_jof" IN_LIST _RA8_APP_LIBS)
     OR ("ra8_rabook_compile" IN_LIST _RA8_APP_LIBS)
  )
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
        ${RA8_REPO_ROOT}/libs/third_party/xz_embedded/xz_dec_stream.c
    )
    list(APPEND _ra8_lib_extra ${_ra8_xz_vendor})
    list(
      APPEND
      _ra8_lib_inc
      ${RA8_REPO_ROOT}/libs/third_party/xz_embedded
      ${RA8_REPO_ROOT}/libs/ra8_unarch/inc
      ${RA8_REPO_ROOT}/libs/third_party/miniz
    )
    if(NOT "ra8_unarch" IN_LIST _RA8_APP_LIBS)
      file(GLOB_RECURSE _ra8_unarch_srcs CONFIGURE_DEPENDS ${RA8_REPO_ROOT}/libs/ra8_unarch/src/*.c)
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
  if(NOT
     (("miniz" IN_LIST _RA8_APP_LIBS)
      OR ("ra8_epub" IN_LIST _RA8_APP_LIBS)
      OR ("ra8_rabook_compile" IN_LIST _RA8_APP_LIBS))
  )
    list(
      FILTER
      _ra8_lib_extra
      EXCLUDE
      REGEX
      "ra8_io/src/ra8_io_compress\\.c$"
    )
    list(
      FILTER
      _ra8_lib_extra
      EXCLUDE
      REGEX
      "ra8_io/src/ra8_io_vfs_compress\\.c$"
    )
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
  # the full "ra8_io" is present, which already compiles these TUs.
  if(("ra8_io_bus" IN_LIST _RA8_APP_LIBS) AND (NOT "ra8_io" IN_LIST _RA8_APP_LIBS))
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
  # ra8_usb_pal). The console-stream binding does not: it hands back an
  # ra8_io_stream_t, so it needs libs/ra8_io/inc AND the ra8_io_stream TUs at
  # link time. Drop it unless the app declared the full "ra8_io", exactly as
  # the ra8_io_compress / ra8_io_blockdev_vsource gates above drop an ra8_io TU
  # whose companion library is absent. "ra8_io_bus" is NOT enough here -- it
  # compiles only the SPI/I2C bus facades, not ra8_io_stream.c. The header
  # (ra8_board_ek_ra8d2_console_stream.h) is likewise kept out of the
  # ra8_board_ek_ra8d2.h umbrella, so an app that never asks for a console
  # stream pays nothing.
  if(NOT "ra8_io" IN_LIST _RA8_APP_LIBS)
    list(
      FILTER
      _ra8_lib_board
      EXCLUDE
      REGEX
      "_console_stream\\.c$"
    )
  endif()

  # The vendored SOUP decoders (miniz DEFLATE, stb image/truetype) type-pun
  # through byte buffers, which violates C strict-aliasing. GCC's aliasing
  # optimizations at -Og/-O2 then miscompile them: arm-none-eabi-gcc 13.3
  # corrupts miniz's inflate so EPUB/RBKC extraction fails (ra8_epub_open ->
  # "FAIL open") on the official toolchain, while older toolchains happen not
  # to trip it. Build just these third_party TUs with -fno-strict-aliasing --
  # the upstream-sanctioned flag for this code -- so the -Og default is
  # correct on every toolchain. First-party sources stay strict-aliasing clean.
  set(_ra8_soup_tu
      ${_ra8_epub_vendor}
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
  # confirmed to fire on a SOUP TU compiled with the full project warning
  # profile under arm-none-eabi-gcc 14.3 at -Og and -O2; none is a memory-safety
  # class.
  set(_ra8_soup_wno_common
      -Wno-cast-qual # miniz / stb cast away const on byte buffers
      -Wno-cast-align # stb_image casts to a wider alignment
      -Wno-double-promotion # stb float -> double in its math hooks
      -Wno-unused-parameter # stb unused formals
      -Wno-type-limits # miniz range-limited comparisons
      -Wno-duplicated-branches # stb_image identical if/else arms
      -Wno-missing-declarations # stb_image extern helpers with no prior decl
      -Wno-conversion # stb / miniz implicit narrowing throughout
      -Wno-sign-conversion # stb / miniz signed<->unsigned index math
      -Wno-float-conversion # stb_truetype double -> float literals
      # stb_image's GIF path (stbi__gif_load / stbi__load_gif_main) puts ~34 KiB
      # of frame buffers on the stack -- far over the per-app -Wstack-usage=N
      # budget, but an inherent property of the vendored decoder we cannot shrink
      # without editing SOUP. The blanket -w hid this too; suppress only the
      # warning here. -fstack-usage still emits the .su data, so the real
      # project-wide stack-bound proof (scripts/checks/stack_usage_check.py over
      # the ARM .su files) still sees these frames.
      -Wno-stack-usage
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
  set(_ra8_elf ${_RA8_APP_NAME}.elf)
endmacro()
