# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Every first-party library source that goes into the ra8_core_hal object
# library, plus the vendored SOUP subsets the host build compiles alongside it.
#
# One place to answer "is this library in the host test build". A source glob
# that quietly stopped matching is invisible -- the build still succeeds, the
# library is simply never compiled or tidied.
#
# Included from tests/CMakeLists.txt. CMake include() is textual within the
# same directory scope, so every variable and target defined here is visible
# to the driver and to the fragments included after it.

# Firmware root (one directory up from tests/).
get_filename_component(FW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# Collect every library source. Each .c file becomes part of the
# `ra8_core_hal` static library. clang-tidy then walks the associated
# compile_commands.json.
file(GLOB_RECURSE RA8_CORE_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_core/src/*.c)
file(GLOB_RECURSE XML_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/apps/shared_libs/xml/src/*.c)
file(GLOB_RECURSE RA8_HAL_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_hal/src/*.c)
# libs/ra8_jpeg has no C sources left: the imgdec backend, the encoder
# (#2798) and the marker walk, whole-buffer decoder and stripe driver (#2799)
# are Zig (libs/ra8_jpeg/src/*.zig, built by libs/ra8_jpeg/build.zig) behind
# the unchanged C headers, and tests/cmake/zig_libraries.cmake links that
# archive at directory scope. There is no RA8_JPEG_SOURCES glob any more.
# ra8_net_pal has no C sources: the frame ring, the ra8_eth status
# translation and the event fan-out are Zig (libs/ra8_net_pal/src/*.zig,
# built by libs/ra8_net_pal/build.zig) behind the unchanged C header, and
# tests/cmake/zig_libraries.cmake links that archive into ra8_core_hal.
# ra8_modem_at has no C sources left: the line accumulator state machine, the
# final-result-code table, the capture appender and the URC dispatch table are
# Zig now (libs/ra8_modem_at/src/*.zig, built by libs/ra8_modem_at/build.zig)
# behind the unchanged C header, and tests/cmake/zig_libraries.cmake links that
# archive into ra8_core_hal. src/ra8_modem_at_internal.h stays: the MC/DC
# suites include it to reach the promoted priv_modem_* helpers.
file(GLOB_RECURSE RA8_TLS_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_tls/src/*.c)
# libs/ra8_usb_pal is fully migrated to Zig: the PAL core, the four descriptor
# builders and the one-call compose facade (#766) all live in
# libs/ra8_usb_pal/src/*.zig, built by libs/ra8_usb_pal/build.zig behind the
# unchanged inc/ra8_usb_pal.h, inc/ra8_usb_desc.h and inc/ra8_usb_compose.h;
# see tests/cmake/zig_libraries.cmake. There is no RA8_USB_PAL_SOURCES glob
# left. src/ra8_usb_pal_internal.h stays: the MC/DC suites include it to reach
# the promoted priv_usb_pal_* predicates.
file(GLOB_RECURSE RA8_FS_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_fs/src/*.c)
# libs/if is fully migrated to Zig: the portable filesystem interface and the
# untrusted-name containment policy (#749) both live in libs/if/src/*.zig,
# built by libs/if/build.zig behind the unchanged inc/fw_if_fs.h,
# inc/fw_if_fs_types.h, inc/fw_if_fs_backend.h and inc/ra8_path.h; see
# tests/cmake/zig_libraries.cmake. There is no RA8_IF_SOURCES glob left, and
# libs/if/src is no longer an include directory anywhere.
# ra8_net_policy is fully migrated to Zig: the URL and peer-address safety
# policy behind inc/ra8_net_urlguard.h lives in libs/ra8_net_policy/src/*.zig,
# built by libs/ra8_net_policy/build.zig; see tests/cmake/zig_libraries.cmake.
# There is no RA8_NET_POLICY_SOURCES glob left.
# ra8_xml is fully migrated to Zig: the bounded XML emitter behind
# inc/ra8_xml_writer.h lives in libs/ra8_xml/src/*.zig, built by
# libs/ra8_xml/build.zig; see tests/cmake/zig_libraries.cmake. There is no
# RA8_XML_WRITER_SOURCES glob left.
# ra8_imgdec is fully migrated to Zig: the decoder seam, the container sniff,
# the geometry probe, the naming table, the bump scratch and the mux behind
# inc/ra8_imgdec*.h live in libs/ra8_imgdec/src/*.zig, built by
# libs/ra8_imgdec/build.zig; see tests/cmake/zig_libraries.cmake. There is no
# RA8_IMGDEC_SOURCES glob left.
# if_ra8_vfs is fully migrated to Zig; see tests/cmake/zig_libraries.cmake.
# Its private contracts header went with the .c, so libs/if_ra8_vfs/src is no
# longer an include directory anywhere.
# if_ra8_cgc arrived from dev in a43342038 (#693) as C and has not been ported
# yet, so unlike the migrated libraries above it still needs its glob.
file(GLOB_RECURSE RA8_IF_RA8_CGC_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/if_ra8_cgc/src/*.c)
file(GLOB_RECURSE RA8_IO_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_io/src/*.c)
file(GLOB_RECURSE COMPRESS_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/apps/shared_libs/compress/src/*.c)
# ra8_audio is fully migrated to Zig (facade + memory and PDM backends);
# see tests/cmake/zig_libraries.cmake. src/ra8_audio_internal.h stays: the
# host suite includes it to build its own fake backend vtable.
# ra8_camera is migrated to Zig apart from one file: the facade, the fixed-frame
# memory source, the JPEG passthrough codec and the software-JPEG codec are Zig
# (see tests/cmake/zig_libraries.cmake). The glob below therefore finds only
# ra8_camera is entirely Zig now (libs/ra8_camera/build.zig): the CEU capture
# backend was the last C translation unit, and the two suites that used to
# white-box it with `#include "ra8_camera_source_ceu.c"` call the
# `priv_cam_ceu_` seams in src/ra8_camera_source_ceu_private.h instead. No .c
# remains to glob, so there is no RA8_CAMERA_SOURCES. libs/ra8_camera/src stays
# an include directory: src/ra8_camera_internal.h and that private header are
# both consumed by the host suites.
# ra8_camera_io is entirely Zig (libs/ra8_camera_io/build.zig): the one
# encode-then-write bridge TU behind inc/ra8_camera_stream.h is ported, so
# src/ has no .c and no private header left. There is no
# RA8_CAMERA_IO_SOURCES; tests/cmake/zig_libraries.cmake links the archive
# into ra8_core_hal and the unchanged C suites cover it through the header.
# ra8_ftl is entirely Zig (libs/ra8_ftl/build.zig): the core, the checkpoint
# and now the mount lifecycle. No C sources remain to glob, so there is no
# RA8_FTL_SOURCES; the unchanged C suites cover it through inc/ra8_ftl.h.
file(GLOB_RECURSE RA8_MEM_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_mem/src/*.c)
# ra8_sdmmc_spi has no C sources: the protocol core AND the block-I/O TU are
# both Zig now (libs/ra8_sdmmc_spi/src/*.zig, built by
# libs/ra8_sdmmc_spi/build.zig) behind the unchanged C header, and
# tests/cmake/zig_libraries.cmake links that archive into ra8_core_hal.
# src/ra8_sdmmc_spi_internal.h stays: two host suites include it to reach
# g_sdmmc_spi_state and the priv_sdmmc_spi_* protocol helpers.
# ra8_gfx has no C sources at all: the rasteriser (the shared framebuffer
# binding g_gfx_text_state, the four promoted helpers and the fifteen entry
# points of inc/ra8_gfx.h, the two bind forms, the teardown, the packed-gray4
# loupe zoom blit and both text calls included) is Zig now
# (libs/ra8_gfx/src/*.zig, built by libs/ra8_gfx/build.zig) behind the
# unchanged C headers, and tests/cmake/zig_libraries.cmake links that archive
# into ra8_core_hal. The per-panel tone LUT joined the archive with #1326, the
# blue-noise dither with #1402, the lifecycle binder with #1466 and the
# bundled 8x16 font table with #2689, which retired this glob: the descriptor
# ra8_gfx_font_8x16 is exported from the archive now, so there is no
# RA8_GFX_SOURCES variable to carry.
# ra8_ui has no C sources: the interaction core (hit-testing, screen stack,
# paging) is Zig (libs/ra8_ui/src/*.zig, built by libs/ra8_ui/build.zig)
# behind the unchanged C header, and tests/cmake/zig_libraries.cmake links
# that archive into ra8_core_hal.
# ra8_keyboard has no C sources: the half-unit key grid, the three layers and
# the typing model are Zig (libs/ra8_keyboard/src/*.zig, built by
# libs/ra8_keyboard/build.zig) behind the unchanged C header, and
# tests/cmake/zig_libraries.cmake links that archive into ra8_core_hal.
# ra8_box has no host C sources: its implementation is Zig (libs/ra8_box/src/
# *.zig, built by libs/ra8_box/build.zig) behind the unchanged C header, and
# tests/cmake/zig_libraries.cmake links that archive into ra8_core_hal. The ARM
# cross build still compiles the retained C implementation through
# ra8_add_app(LIBS ra8_box) until the Zig cross-build wiring lands.
file(GLOB_RECURSE BOOK_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/apps/shared_libs/book/src/*.c)
file(GLOB_RECURSE RABOOK_COMPILE_SOURCES CONFIGURE_DEPENDS
     ${FW_ROOT}/apps/shared_libs/rabook_compile/src/*.c
)
set(RABOOK_COMPILE_CPP_SOURCES "")
file(GLOB_RECURSE RABOOK_IMPORT_SOURCES CONFIGURE_DEPENDS
     ${FW_ROOT}/apps/shared_libs/rabook_import/src/*.c
)
# ra8_batt is implemented in Zig (libs/ra8_batt/build.zig).
# ra8_widget is entirely Zig now: the flat op core (rect maths, invalidate,
# damage, the dispatch every widget routes through, render_dirty), the RA8_PRIV
# paint helpers, seven leaf widgets (text label, push button, progress bar,
# status bar, toolbar, on-screen keyboard, navigation strip), the container
# panel that nests them into a tree, the paged reflow view and the book-card
# grid are all libs/ra8_widget/src/*.zig, built by libs/ra8_widget/build.zig and
# linked through cmake/zig_libraries.cmake as ra8_zig::ra8_widget. src/ra8_widget.c
# and the private src/ra8_widget_internal.h are deleted with this port, so
# libs/ra8_widget/src carries no C at all and there is nothing left to glob. The
# unchanged C suites still cover the library through its public headers.
# ra8_app is implemented in Zig (libs/ra8_app/build.zig). It is linked through
# cmake/zig_libraries.cmake instead of being globbed as C sources here; the
# unchanged C suite still covers it via the public header.
file(GLOB_RECURSE RA8_NSC_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_nsc/src/*.c)
# ra8_ota is fully implemented in Zig now (ra8_zig::ra8_ota): the parsing and
# validation cluster, the orchestration state machine and the verify cluster.
# libs/ra8_ota/src carries no C at all, so there is no glob here; the private
# src/ra8_ota_internal.h stays because three C suites include it.
# ra8_display_pal is entirely Zig now: the dispatcher, the refresh policy and
# both panel backends are libs/ra8_display_pal/src/*.zig (built by
# libs/ra8_display_pal/build.zig) and linked through cmake/zig_libraries.cmake.
# No C sources are left to glob, so RA8_DISPLAY_PAL_SOURCES stays empty and the
# unchanged C suites cover the library through its public headers.
# ra8_power_profile is implemented in Zig (libs/ra8_power_profile/build.zig).
# It is linked through cmake/zig_libraries.cmake instead of being globbed as C
# sources here; the unchanged C suite still covers it via the public header.
file(GLOB_RECURSE EPUB_C_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/apps/shared_libs/epub/src/*.c)
set(EPUB_CPP_SOURCES "")
file(GLOB_RECURSE COMIC_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/apps/shared_libs/comic/src/*.c)
file(GLOB_RECURSE UNARCH_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/apps/shared_libs/unarch/src/*.c)
# xz-embedded decode-only SOUP: exactly the TUs the XZ wrapper drives. They
# include the first-party porting header apps/shared_libs/unarch/inc/xz_config.h
# (allocator seam + mode selection), so that include dir must stay on the
# ra8_core_hal PUBLIC list.
set(RA8_XZ_THIRD_PARTY
    ${FW_ROOT}/apps/shared_libs/third_party/xz_embedded/xz_crc32.c
    ${FW_ROOT}/apps/shared_libs/third_party/xz_embedded/xz_crc64.c
    ${FW_ROOT}/apps/shared_libs/third_party/xz_embedded/xz_dec_lzma2.c
    ${FW_ROOT}/apps/shared_libs/third_party/xz_embedded/xz_dec_stream.c
)
file(GLOB_RECURSE JOF_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/apps/shared_libs/jof/src/*.c)
file(GLOB_RECURSE LONGSTRIP_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/apps/shared_libs/longstrip/src/*.c)
file(GLOB_RECURSE ZOOM_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/apps/shared_libs/zoom/src/*.c)
# reflow has two implementations: v1 (hand-rolled, default) and v2
# (LiteHTML-backed, REFLOW_USE_LITEHTML=ON). Only one set is compiled.
option(REFLOW_USE_LITEHTML "Use the LiteHTML v2 reflow engine" OFF)
if(REFLOW_USE_LITEHTML)
  # The static stb_truetype arena allocator (ra8_stbtt_malloc/free) is shared:
  # both v1 and the litehtml v2 engine rasterise glyphs through stb_truetype,
  # so this one file stays in even when the v1 reflow engine is excluded.
  # ra8_stbtt_guard.c (the #217 sfnt table-directory bounds check) is likewise
  # shared: epub's priv_font_init calls it before stbtt_InitFont, so its
  # symbol must resolve even under the v2 engine.
  # ra8_img_arena.c (the stb_image bump arena) is likewise shared: the always-on
  # stb_image_impl.c TU in EPUB_THIRD_PARTY references its symbols.
  # reflow_link.c is pure query logic over engine fields (no layout/stbtt),
  # so the #110 link/anchor API is available under v2 as well.
  set(REFLOW_C_SOURCES
      ${FW_ROOT}/apps/shared_libs/reflow/src/ra8_stbtt_alloc.c
      ${FW_ROOT}/apps/shared_libs/reflow/src/ra8_stbtt_guard.c
      ${FW_ROOT}/apps/shared_libs/reflow/src/ra8_img_arena.c
      ${FW_ROOT}/apps/shared_libs/reflow/src/reflow_link.c
  )
  set(REFLOW_CPP_SOURCES ${FW_ROOT}/apps/shared_libs/reflow/v2/src/reflow_v2.cpp)
  enable_language(CXX)
  set(_RA8_SAVED_C_FLAGS "${CMAKE_C_FLAGS}")
  set(_RA8_SAVED_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  string(REPLACE "-Werror" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
  string(REPLACE "-Werror" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  if(NOT TARGET litehtml)
    add_subdirectory(
      ${FW_ROOT}/apps/shared_libs/third_party/litehtml ${CMAKE_BINARY_DIR}/_litehtml
      EXCLUDE_FROM_ALL
    )
  endif()
  if(TARGET litehtml)
    # One measured flag. All three names were tested on their own across the 60
    # LiteHTML C++ TUs under both pinned host compilers. Only
    # -Wdeprecated-declarations fires, and only under clang 18: LiteHTML's
    # render-item pool calls std::get_temporary_buffer, removed in C++17 and
    # deprecated in libstdc++'s stl_tempbuf.h. -Wno-error masked nothing --
    # -Werror is stripped from CMAKE_C_FLAGS / CMAKE_CXX_FLAGS a few lines above
    # precisely so this vendored subdirectory does not inherit it, and LiteHTML's
    # own listfile sets -Wall -Wextra -Wpedantic without -Werror -- and
    # -Wno-void-pointer-to-enum-cast fired on neither compiler. Both are deleted.
    target_compile_options(
      litehtml PRIVATE -Wno-deprecated-declarations # clang-18 diagnoses std::get_temporary_buffer.
    )
    # The first-party reflow_v2 wrapper includes LiteHTML's public headers.
    # Treat those vendored headers as SYSTEM so their diagnostics do not
    # require suppressing warnings on the wrapper translation unit itself.
    target_include_directories(
      litehtml SYSTEM
      INTERFACE $<BUILD_INTERFACE:${FW_ROOT}/apps/shared_libs/third_party/litehtml/src>
                $<BUILD_INTERFACE:${FW_ROOT}/apps/shared_libs/third_party/litehtml/include>
    )
  endif()
  if(TARGET gumbo)
    # No warning suppression: gumbo carried -Wno-error and -Wno-unused-parameter
    # and neither masked anything. Its whole compile line is the toolchain
    # defaults plus those two flags -- no -Wall, no -Wextra, so
    # -Wunused-parameter is never enabled, and no -Werror for -Wno-error to
    # undo. Compiling all 11 gumbo TUs with both removed emits nothing under
    # gcc 14.2.0 or clang 18.
    target_include_directories(
      gumbo SYSTEM
      INTERFACE
        $<BUILD_INTERFACE:${FW_ROOT}/apps/shared_libs/third_party/litehtml/src/gumbo/include>
    )
  endif()
  set(CMAKE_C_FLAGS "${_RA8_SAVED_C_FLAGS}")
  set(CMAKE_CXX_FLAGS "${_RA8_SAVED_CXX_FLAGS}")
else()
  file(GLOB_RECURSE REFLOW_C_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/apps/shared_libs/reflow/src/*.c)
  file(GLOB_RECURSE REFLOW_CPP_SOURCES CONFIGURE_DEPENDS
       ${FW_ROOT}/apps/shared_libs/reflow/src/*.cpp
  )
endif()
set(EPUB_THIRD_PARTY
    ${FW_ROOT}/apps/shared_libs/third_party/miniz/miniz.c
    ${FW_ROOT}/apps/shared_libs/third_party/stb/stb_truetype_impl.c
    ${FW_ROOT}/apps/shared_libs/third_party/stb/stb_image_impl.c
)
# libwebp decode-only SOUP (#290) + the first-party ra8_webp facade/arena that
# fronts it. Only the decoder subset is vendored under
# apps/shared_libs/third_party/libwebp,
# so a recursive *.c glob is exactly that subset. NOT yet wired into the
# reflow/ra8_img raster dispatch (that is #289) -- compiled into ra8_core_hal
# so the standalone WebP decode host test (test_ra8_webp.c) and the fuzz harness
# (fuzz_ra8_webp) link against it. utils.c routes its allocator through the
# ra8_webp bump arena via -DRA8_WEBP_USE_ARENA (set below).
include(${FW_ROOT}/cmake/ra8_webp_vendor.cmake)
ra8_webp_vendor_sources(RA8_WEBP_THIRD_PARTY ${FW_ROOT})
ra8_webp_facade_sources(RA8_WEBP_SOURCES ${FW_ROOT})
# ra8_secure_app has no C sources left: the key vault, the entropy read, the
# OTA bank commit, the AES-CMAC and the sealed-key import (#2670) are Zig
# now, linked via tests/cmake/zig_libraries.cmake. inc/ and the three
# src/*_internal.h headers stay: the NSC veneers and the C security suites
# include them for the constants and the priv_ declarations.
# ra8_psa_crypto has no C sources left: the key-slot pool, the guard order,
# the PSA vocabulary mapping and both backends (the deterministic off-target
# fake and the on-target tf-psa-crypto binding) are Zig now, linked via
# tests/cmake/zig_libraries.cmake. inc/ and src/ra8_psa_crypto_internal.h
# stay: the two C security suites include the internal header for
# struct ra8_psa_key_handle and k_ra8_psa_fake_scratch_bytes.
# ra8_wdt_supervisor has no C sources left: the registry, the deadline policy
# and the ThreadX seam are Zig now, linked via tests/cmake/zig_libraries.cmake.
# ra8_mpu has no C sources left: the descriptor validation, the RBAR/RLAR
# encoding and the canonical boot attribute map are Zig now, linked via
# tests/cmake/zig_libraries.cmake.
file(GLOB RA8_BOARD_EK_RA8D2_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_board_ek_ra8d2/src/*.c)
# ra8_lsm6dso has no C sources left: the register-level driver (the CTRL1_XL /
# CTRL2_G encoders, the little-endian sample decoders, the temperature
# conversion and the FIFO drain) AND the house-I2C binder from #760 are all
# Zig now, linked via tests/cmake/zig_libraries.cmake. The four binder cases
# in test_ra8_lsm6dso.c take ra8_lsm6dso_bind_i2c from that archive, which
# ra8_core_hal links PUBLIC and which is attached at directory scope for the
# object-library consumers; imu_lsm6dso_demo names ra8_lsm6dso in LIBS, so the
# app recipe registers the same archive for it.
# ra8_ov5640 has no C sources left: the register protocol, the qualified VGA
# DVP scene table, the JPEG overlay, the status decode AND the house-I2C binder
# from #760 are all Zig now, linked via tests/cmake/zig_libraries.cmake. The
# four vectors in tests/graphics/src/test_ra8_ov5640_bind.c take
# ra8_ov5640_bind_i2c from that archive, which ra8_core_hal links PUBLIC and
# which is attached at directory scope for the object-library consumers.
# ra8_tz_secure_boot has no C sources left: the secure-boot sequence, the SAU
# and IPC partitioning, the PSAR gate and the NS root-of-trust reader are all
# Zig now, linked via tests/cmake/zig_libraries.cmake. The NS-side
# ns/ra8_ns_rot_header.c is a different artifact: it is data compiled into the
# Non-Secure image by ra8_add_ns_image.cmake, never into this library.
# ra8_dfu is partially migrated. The polled host-side DFU driver is Zig
# (#2809) and this glob no longer matches it, but the boot decision, the
# MRAM program/verify path, the USBX device class, the launch gate, the
# anti-rollback counter and the root-of-trust reader are all still C, so the
# glob stays non-empty and issue #908 does not bite here. The Zig archive is
# deliberately NOT linked into ra8_core_hal: the C it replaces was firmware-only
# (#ifndef RA8_OFF_TARGET), so it contributed nothing to this host build, and its
# ra8_usb_host_* seam has no host-side implementation to bind to. The ARM side
# gets the archive through cmake/ra8_app/sources.cmake instead.
file(GLOB_RECURSE RA8_DFU_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_dfu/src/*.c)
# ra8_devcfg has no C sources left: both the record core and the production
# extra-MRAM store binding are Zig now, linked via tests/cmake/zig_libraries.cmake.
# ra8_wifi: fully migrated. The facade and the ESP32-C6 backend are both Zig
# (libs/ra8_wifi/src has no .c left), linked via
# tests/cmake/zig_libraries.cmake, so ra8_core_hal globs nothing for this
# library and tests/cmake/tests_wifi.cmake compiles no ra8_wifi translation
# unit either. The backend still rides ra8_c6link + the vendored protobuf
# codec, which ra8_core_hal does not carry, so the c6 host test keeps its own
# target there and links the archive by name.
