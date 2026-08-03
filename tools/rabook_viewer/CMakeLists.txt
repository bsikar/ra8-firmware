#
# tools/rabook_viewer -- native macOS reader viewer (direct-call, not an emulator).
#
# Links the firmware's platform-agnostic reader libraries compiled host-side
# (RA8_OFF_TARGET, C23) and calls them directly to rasterise e-book / comic
# pages into an RGB565 framebuffer, which a layer-backed Cocoa NSWindow presents.
# Unlike tools/ra8_emulator (which emulates the firmware ELF under Unicorn), this
# tool runs no ARM code -- it is purely a viewer, so it is far faster.
#
#   cmake -B build -S . && cmake --build build -j
#   ./build/rabook_viewer path/to/book.cbz               # window; arrows turn pages
#   ./build/rabook_viewer book.cbz --headless --dump-ppm page0.ppm   # no display
#
# Reader engine coverage: CBZ / CBR / CBT comic archives incl. gzip/xz-wrapped
# (ra8_comic), and the firmware-native JOF long-strip vertical-scroll tile atlas
# (ra8_longstrip). EPUB / RABOOK reflow (text + image loader over ra8_reflow) are
# left as TODO seams (see src/ra8_viewer_reader.c). macOS only -- the window
# backend is Cocoa; other hosts are not a target for this dev tool.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

cmake_minimum_required(VERSION 3.20)

project(rabook_viewer LANGUAGES C)
if(APPLE)
  enable_language(OBJC)
endif()

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

#Firmware root(two directories up from tools / rabook_viewer /).
get_filename_component(FW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# ---------------------------------------------------------------------------
# ra8_viewer_read_path(<out> <lib_src_dir> KEEP <files...> DROP <files...>)
#
# Resolve -- and CHECK -- the read-path members of a firmware library the viewer
# links only a SUBSET of. KEEP names the reader-core members compiled into
# ra8_reader; DROP names the producer / encoder / block-device / text-layout
# members deliberately left out. The function globs <lib_src_dir>/*.c and fails
# configuration unless KEEP and DROP partition that glob exactly:
#
#   * a present file in neither list -- a reader-core file was split, renamed or
#     added; classify it KEEP (link it) or DROP (leave it out);
#   * a listed file that is not present -- a member was renamed or deleted;
#     update its KEEP/DROP list.
#
# Either way `cmake` fails LOUDLY at configure time -- which the tools-build gate
# runs under clang-18 and gcc-14 -- so a frozen hand-list can no longer rot into
# a silent undefined-symbol link error found only by a later manual build. <out>
# receives the KEEP members as absolute paths.
# ---------------------------------------------------------------------------
function(ra8_viewer_read_path out_var lib_src_dir)
  cmake_parse_arguments(
    _RP
    ""
    ""
    "KEEP;DROP"
    ${ARGN}
  )

  file(GLOB _present_abs CONFIGURE_DEPENDS ${lib_src_dir}/*.c)
  set(_present "")
  foreach(_path IN LISTS _present_abs)
    get_filename_component(_name "${_path}" NAME)
    list(APPEND _present "${_name}")
  endforeach()

  set(_declared ${_RP_KEEP} ${_RP_DROP})
  foreach(_name IN LISTS _present)
    if(NOT _name IN_LIST _declared)
      message(
        FATAL_ERROR
          "rabook_viewer: ${lib_src_dir}/${_name} is neither a KEEP (read-path) nor "
          "a DROP (excluded) source. A reader-core file was split, renamed or "
          "added -- classify it so the viewer links the right members."
      )
    endif()
  endforeach()

  foreach(_name IN LISTS _declared)
    if(NOT _name IN_LIST _present)
      message(
        FATAL_ERROR "rabook_viewer: declared source ${lib_src_dir}/${_name} does not exist. A "
                    "firmware member was renamed or deleted -- update its KEEP/DROP list."
      )
    endif()
  endforeach()

  set(_keep_abs "")
  foreach(_name IN LISTS _RP_KEEP)
    list(APPEND _keep_abs "${lib_src_dir}/${_name}")
  endforeach()
  set(${out_var}
      "${_keep_abs}"
      PARENT_SCOPE
  )
endfunction()

# ---------------------------------------------------------------------------
# ra8_reader: static archive of exactly the firmware reader sources the viewer
# calls. Building an archive (not an object library) means the linker pulls only
# the members needed to resolve the viewer's references -- transitively -- so the
# HAL / RTOS / peripheral libraries are never dragged in. Under RA8_OFF_TARGET
# the CBZ backend uses miniz's default (malloc) allocator, so no ra8_epub source
# is needed -- only its header (ra8_epub_miniz_alloc.h) sits on the include path.
#
# The source list is DERIVED, never hand-frozen. Libraries whose whole src/ tree
# is read path are globbed (a split or new read-path file is absorbed
# automatically); libraries the viewer links a subset of go through
# ra8_viewer_read_path(), which fails configuration the moment its declaration
# stops covering the library.
# ---------------------------------------------------------------------------

# Whole-read-path libraries -- every src/ member is part of the reader path, so
# a glob is the single source of truth (the host-test build and ra8_emulator
# consume their firmware libraries the same way). ra8_comic pulls in the
# gzip/xz/tar wrapped-container decoders (ra8_unarch, which sits on the vendored
# xz-embedded decoder); ra8_longstrip renders the firmware-native `.jof`
# vertical-scroll tile atlas.
file(GLOB RA8_COMIC_SRC CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_comic/src/*.c)
file(GLOB RA8_GFX_SRC CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_gfx/src/*.c)
file(GLOB RA8_UNARCH_SRC CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_unarch/src/*.c)
file(GLOB RA8_LONGSTRIP_SRC CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_longstrip/src/*.c)

# Partial-read-path libraries -- the viewer links only the read members. The JOF
# strip parses through ra8_jof (DEFLATE tiles inflate via ra8_io_compress over
# the already-linked miniz) and pages through the ra8_mem tile cache (+ its
# keycache); ra8_reflow supplies the image blit + SVG rasteriser (no CSS/text
# layout); ra8_core supplies logging, the error handler and the shared
# decompression-limits policy. Each DROP is producer / encoder / HAL /
# text-layout code the viewer must not link; ra8_viewer_read_path() proves the
# split is still complete.
ra8_viewer_read_path(
  RA8_JOF_SRC ${FW_ROOT}/libs/ra8_jof/src
  KEEP ra8_jof.c
  DROP ra8_jof_png.c ra8_jof_png_chunk.c ra8_jof_produce.c ra8_jof_produce_webp.c
)

ra8_viewer_read_path(
  RA8_IO_SRC ${FW_ROOT}/libs/ra8_io/src
  KEEP ra8_io_compress.c
  DROP ra8_io_blockdev.c
       ra8_io_blockdev_cache.c
       ra8_io_blockdev_mram.c
       ra8_io_blockdev_ram.c
       ra8_io_blockdev_sdhi.c
       ra8_io_blockdev_sdram.c
       ra8_io_blockdev_sdspi.c
       ra8_io_blockdev_vsource.c
       ra8_io_blockdev_xspi.c
       ra8_io_fsfmt.c
       ra8_io_i2c_bus.c
       ra8_io_i2c_bus_i3c_compat.c
       ra8_io_i2c_bus_riic.c
       ra8_io_log.c
       ra8_io_spi_bus.c
       ra8_io_spi_bus_sci_spi.c
       ra8_io_spi_bus_spi_b.c
       ra8_io_stream.c
       ra8_io_stream_blockdev.c
       ra8_io_stream_ram.c
       ra8_io_stream_uart.c
       ra8_io_stream_usbcdc.c
       ra8_io_vfs.c
       ra8_io_vfs_compress.c
)

ra8_viewer_read_path(
  RA8_MEM_SRC ${FW_ROOT}/libs/ra8_mem/src
  KEEP ra8_tile_cache.c ra8_keycache.c
  DROP ra8_arena.c
       ra8_glyph_atlas.c
       ra8_slab.c
       ra8_vmem.c
       ra8_vmem_stream.c
       ra8_vsource.c
)

ra8_viewer_read_path(
  RA8_REFLOW_SRC ${FW_ROOT}/libs/ra8_reflow/src
  KEEP ra8_reflow_image.c
       ra8_img_arena.c
       ra8_reflow_svg.c
       ra8_reflow_svg_doc.c
       ra8_reflow_svg_path.c
       ra8_reflow_svg_shape.c
       ra8_reflow_svg_xform.c
  DROP ra8_reflow_cache.c
       ra8_reflow_css.c
       ra8_reflow_css_cascade.c
       ra8_reflow_css_rules.c
       ra8_reflow_layout.c
       ra8_reflow_layout_driver.c
       ra8_reflow_layout_image.c
       ra8_reflow_layout_table.c
       ra8_reflow_link.c
       ra8_reflow_parse.c
       ra8_reflow_render.c
       ra8_reflow_tokenize.c
       ra8_reflow_tokenize_attr.c
       ra8_reflow_tokenize_lex.c
       ra8_stbtt_alloc.c
       ra8_stbtt_guard.c
)

ra8_viewer_read_path(
  RA8_CORE_SRC ${FW_ROOT}/libs/ra8_core/src
  KEEP ra8_log.c ra8_error_handler.c ra8_decomp_limits.c
  DROP ra8_crashlog.c
       ra8_exception.c
       ra8_infrastructure.c
       ra8_pin_validator.c
       ra8_rand_stub.c
       ra8_sbrk_trap.c
       ra8_scb.c
       ra8_secure.c
       ra8_systick.c
       ra8_time.c
)

file(GLOB RA8_XZ_EMBEDDED_SRC CONFIGURE_DEPENDS ${FW_ROOT}/libs/third_party/xz_embedded/*.c)
set(RA8_THIRD_PARTY_SRC ${FW_ROOT}/libs/third_party/miniz/miniz.c
                        ${FW_ROOT}/libs/third_party/stb/stb_image_impl.c ${RA8_XZ_EMBEDDED_SRC}
)

add_library(
  ra8_reader STATIC
  ${RA8_COMIC_SRC}
  ${RA8_GFX_SRC}
  ${RA8_UNARCH_SRC}
  ${RA8_LONGSTRIP_SRC}
  ${RA8_JOF_SRC}
  ${RA8_IO_SRC}
  ${RA8_MEM_SRC}
  ${RA8_REFLOW_SRC}
  ${RA8_CORE_SRC}
  ${RA8_THIRD_PARTY_SRC}
)
target_compile_definitions(ra8_reader PUBLIC RA8_OFF_TARGET)
# The firmware sources in this archive are first-party and carry the project
# warning bar (NASA Power of 10 Rule 10) here exactly as they do in the ARM and
# host-test builds. The vendored decoders below override it with -w, which is
# where warning suppression legitimately stops.
target_compile_options(ra8_reader PRIVATE -Wall -Wextra -Werror)
target_include_directories(
  ra8_reader
  PUBLIC ${FW_ROOT}/libs/ra8_core/inc
         ${FW_ROOT}/libs/ra8_gfx/inc
         ${FW_ROOT}/libs/ra8_comic/inc
         ${FW_ROOT}/libs/ra8_reflow/inc
         ${FW_ROOT}/libs/ra8_epub/inc
         ${FW_ROOT}/libs/ra8_unarch/inc
         ${FW_ROOT}/libs/ra8_longstrip/inc
         ${FW_ROOT}/libs/ra8_jof/inc
         ${FW_ROOT}/libs/ra8_io/inc
         ${FW_ROOT}/libs/ra8_mem/inc
         ${FW_ROOT}/libs/third_party/miniz
         ${FW_ROOT}/libs/third_party/stb
         ${FW_ROOT}/libs/third_party/xz_embedded
)
# The vendored decoders (miniz, stb_image) type-pun through byte buffers; silence
# their warnings and disable strict aliasing exactly as the host-test build does.
set_source_files_properties(
  ${RA8_THIRD_PARTY_SRC} ${RA8_XZ_EMBEDDED_SRC} PROPERTIES COMPILE_OPTIONS
                                                           "-w;-fno-strict-aliasing"
)

# ---------------------------------------------------------------------------
# rabook_viewer: the viewer executable -- C core + Cocoa window backend.
# ---------------------------------------------------------------------------
# The window backend is the only platform-specific piece. Cocoa on macOS; off
# the APPLE path a headless stand-in supplies the identical C ABI and reports
# "no window" so main.c takes its existing headless branch. Exactly one of the
# two is ever compiled, so they never compete for the same symbols -- the same
# arrangement tools/ra8_emulator uses for board_view.m.
if(APPLE)
  set(RA8_VIEWER_VIEW_SRC src/ra8_viewer_view.m)
else()
  set(RA8_VIEWER_VIEW_SRC src/ra8_viewer_view_stub.c)
endif()

# The reader core is one module split across four TUs (see
# src/ra8_viewer_reader_internal.h): public API + dispatch, the comic engine, the
# JOF long-strip engine, and the shared RGB565 / PPM writer.
add_executable(
  rabook_viewer
  src/main.c
  src/ra8_viewer_reader.c
  src/ra8_viewer_comic.c
  src/ra8_viewer_jof.c
  src/ra8_viewer_ppm.c
  ${RA8_VIEWER_VIEW_SRC}
)
target_include_directories(rabook_viewer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc)
target_link_libraries(rabook_viewer PRIVATE ra8_reader)
target_compile_options(rabook_viewer PRIVATE -Wall -Wextra -Werror)

if(APPLE)
  set_source_files_properties(src/ra8_viewer_view.m PROPERTIES COMPILE_FLAGS "-fobjc-arc")
  target_link_libraries(rabook_viewer PRIVATE "-framework Cocoa" "-framework QuartzCore")
else()
  # The SVG rasteriser calls sinf/cosf/tanf/sqrtf/atan2f. macOS keeps those in
  # libSystem; every other POSIX host needs libm named explicitly.
  target_link_libraries(rabook_viewer PRIVATE m)
endif()
