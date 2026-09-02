# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# tools/rabook_viewer -- native macOS reader viewer (direct-call, not an emulator).
#
# Links the firmware's platform-agnostic JOF and comic libraries compiled
# host-side (RA8_OFF_TARGET, C23) and calls them directly to rasterise tiles into
# an RGB565 framebuffer, which a layer-backed Cocoa NSWindow presents.
# Unlike tools/ra8_emulator (which emulates the firmware ELF under Unicorn), this
# tool runs no ARM code -- it is purely a viewer, so it is far faster.
#
#   cmake -B build -S . && cmake --build build -j
#   ./build/rabook_viewer path/to/book.cbz               # window; arrows turn pages
#   ./build/rabook_viewer book.jof --headless --dump-ppm page0.ppm   # no display
#
# Reader engine coverage is bare CBZ/CBR/CBT plus the firmware-native JOF
# long-strip atlas. Every comic buffer is a bounded caller-workspace slice.
# Gzip/XZ-wrapped comics remain rejected until their wrapper API can stream or
# descriptor-spool output instead of retaining the whole archive.
#
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
# HAL / RTOS / peripheral libraries are never dragged in. Miniz is retained for
# raw-DEFLATE JOF bands, with its malloc/stdio/time paths disabled here.
#
# The source list is DERIVED, never hand-frozen. Libraries whose whole src/ tree
# is read path are globbed (a split or new read-path file is absorbed
# automatically); libraries the viewer links a subset of go through
# ra8_viewer_read_path(), which fails configuration the moment its declaration
# stops covering the library.
# ---------------------------------------------------------------------------

# Whole-read-path libraries -- every src/ member is part of the reader path, so
# a glob is the single source of truth (the host-test build and ra8_emulator
# consume their firmware libraries the same way). longstrip renders the
# firmware-native `.jof` vertical-scroll tile atlas.
file(GLOB LONGSTRIP_SRC CONFIGURE_DEPENDS ${FW_ROOT}/apps/shared_libs/longstrip/src/*.c)
file(GLOB COMIC_SRC CONFIGURE_DEPENDS ${FW_ROOT}/apps/shared_libs/comic/src/*.c)
file(GLOB RA8_GFX_SRC CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_gfx/src/*.c)

# Partial-read-path libraries -- the viewer links only the read members. The JOF
# strip parses through jof (DEFLATE tiles inflate via ra8_compress over
# the already-linked miniz) and pages through the ra8_mem tile cache (+ its
# keycache); ra8_core supplies logging, the error handler and the shared
# decompression-limits policy. Each DROP is producer / encoder / HAL code the
# viewer must not link; ra8_viewer_read_path() proves the split is complete.
ra8_viewer_read_path(
  JOF_SRC ${FW_ROOT}/apps/shared_libs/jof/src
  KEEP jof.c
  DROP jof_audit.c
       jof_png.c
       jof_png_chunk.c
       jof_produce.c
       jof_produce_webp.c
)

ra8_viewer_read_path(
  EPUB_SRC ${FW_ROOT}/apps/shared_libs/epub/src
  KEEP epub_miniz_alloc.c
  DROP epub_chapter.c
       epub_entry.c
       epub_fs.c
       epub_img_import.c
       epub_img_tiles.c
       epub_open.c
       epub_xml_shim.c
       epub_xml_toc.c
       epub_zip_guard.c
)

ra8_viewer_read_path(
  REFLOW_SRC ${FW_ROOT}/apps/shared_libs/reflow/src
  KEEP ra8_img_arena.c
       reflow_image.c
       reflow_svg.c
       reflow_svg_doc.c
       reflow_svg_path.c
       reflow_svg_shape.c
       reflow_svg_xform.c
  DROP reflow_cache.c
       reflow_css.c
       reflow_css_cascade.c
       reflow_css_rules.c
       reflow_layout.c
       reflow_layout_driver.c
       reflow_layout_image.c
       reflow_layout_table.c
       reflow_link.c
       reflow_parse.c
       reflow_render.c
       reflow_tokenize.c
       reflow_tokenize_attr.c
       reflow_tokenize_lex.c
       ra8_stbtt_alloc.c
       ra8_stbtt_guard.c
)

ra8_viewer_read_path(
  UNARCH_SRC ${FW_ROOT}/apps/shared_libs/unarch/src
  KEEP unarch_io.c unarch_tar.c unarch_tar_fields.c
  DROP unarch_gzip.c unarch_xz.c unarch_xz_pool.c
)

ra8_viewer_read_path(
  RA8_IO_SRC ${FW_ROOT}/libs/ra8_io/src
  KEEP ra8_io_stream.c
  DROP ra8_io_blockdev.c
       ra8_io_blockdev_cache.c
       ra8_io_blockdev_mram.c
       ra8_io_blockdev_ram.c
       ra8_io_blockdev_sdhi.c
       ra8_io_blockdev_sdram.c
       ra8_io_blockdev_sdspi.c
       ra8_io_blockdev_usbmsc.c
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
       ra8_io_stream_blockdev.c
       ra8_io_stream_ram.c
       ra8_io_stream_uart.c
       ra8_io_stream_usbcdc.c
       ra8_io_vfs.c
       ra8_io_vfs_namespace.c
)

ra8_viewer_read_path(
  COMPRESS_SRC ${FW_ROOT}/apps/shared_libs/compress/src
  KEEP ra8_compress.c
  DROP ra8_vfs_compress.c
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

set(RA8_THIRD_PARTY_SRC ${FW_ROOT}/apps/shared_libs/third_party/miniz/miniz.c
                        ${FW_ROOT}/apps/shared_libs/third_party/stb/stb_image_impl.c
)
set(RA8_STREAM_POSIX_SRC ${FW_ROOT}/port/posix/src/ra8_io_stream_posix.c)
set(RA8_STREAM_RAM_SRC ${FW_ROOT}/libs/ra8_io/src/ra8_io_stream_ram.c)

add_library(
  ra8_reader STATIC
  ${COMIC_SRC}
  ${RA8_GFX_SRC}
  ${UNARCH_SRC}
  ${LONGSTRIP_SRC}
  ${EPUB_SRC}
  ${REFLOW_SRC}
  ${JOF_SRC}
  ${RA8_IO_SRC}
  ${COMPRESS_SRC}
  ${RA8_MEM_SRC}
  ${RA8_CORE_SRC}
  ${RA8_THIRD_PARTY_SRC}
)
target_compile_definitions(
  ra8_reader PUBLIC MINIZ_NO_MALLOC MINIZ_NO_STDIO MINIZ_NO_TIME RA8_OFF_TARGET
)
# The firmware sources in this archive are first-party and carry the project
# warning bar (NASA Power of 10 Rule 10) here exactly as they do in the ARM and
# host-test builds. The vendored decoders below override it with -w, which is
# where warning suppression legitimately stops.
target_compile_options(ra8_reader PRIVATE -Wall -Wextra -Werror)
target_include_directories(
  ra8_reader
  PUBLIC ${FW_ROOT}/libs/ra8_core/inc
         ${FW_ROOT}/apps/shared_libs/comic/inc
         ${FW_ROOT}/apps/shared_libs/epub/inc
         ${FW_ROOT}/libs/ra8_gfx/inc
         ${FW_ROOT}/apps/shared_libs/longstrip/inc
         ${FW_ROOT}/apps/shared_libs/jof/inc
         ${FW_ROOT}/libs/ra8_io/inc
         ${FW_ROOT}/apps/shared_libs/compress/inc
         ${FW_ROOT}/libs/ra8_mem/inc
         ${FW_ROOT}/apps/shared_libs/reflow/inc
         ${FW_ROOT}/apps/shared_libs/unarch/inc
         ${FW_ROOT}/apps/shared_libs/third_party/miniz
         ${FW_ROOT}/apps/shared_libs/third_party/stb
)
# The vendored miniz decoder type-puns through byte buffers. Removing every
# warning control under both pinned compiler arms found only two unused
# callback parameters; all ten other warning controls and the blanket demotion
# were dead. Keep that exact diagnostic class and the optimizer contract.
set(_ra8_miniz_compile_options
    -Wno-unused-parameter # miniz callbacks leave last_modified/pFile_time unused.
    -fno-strict-aliasing
)
set_source_files_properties(
  ${RA8_THIRD_PARTY_SRC} PROPERTIES COMPILE_OPTIONS "${_ra8_miniz_compile_options}"
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

# The reader core is split across public dispatch, comic and JOF engines, and
# the shared RGB565 / descriptor-backed PPM writer.
add_executable(
  rabook_viewer
  src/main.c
  src/ra8_viewer_output.c
  src/ra8_viewer_reader.c
  src/ra8_viewer_comic.c
  src/ra8_viewer_jof.c
  src/ra8_viewer_ppm.c
  ${RA8_STREAM_POSIX_SRC}
  ${RA8_VIEWER_VIEW_SRC}
)
target_include_directories(
  rabook_viewer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc ${FW_ROOT}/port/posix/inc
)
target_link_libraries(rabook_viewer PRIVATE ra8_reader)
target_compile_options(rabook_viewer PRIVATE -Wall -Wextra -Werror)

include(CTest)
if(BUILD_TESTING)
  add_executable(
    test_viewer_workspace
    tests/src/test_viewer_workspace.c
    src/ra8_viewer_reader.c
    src/ra8_viewer_comic.c
    src/ra8_viewer_jof.c
    src/ra8_viewer_ppm.c
  )
  target_include_directories(test_viewer_workspace PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc)
  target_link_libraries(test_viewer_workspace PRIVATE ra8_reader)
  target_compile_options(test_viewer_workspace PRIVATE -Wall -Wextra -Werror)
  add_executable(
    test_viewer_comic_workspace
    tests/src/test_viewer_comic_workspace.c
    src/ra8_viewer_reader.c
    src/ra8_viewer_comic.c
    src/ra8_viewer_jof.c
    src/ra8_viewer_ppm.c
  )
  target_include_directories(test_viewer_comic_workspace PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc)
  target_link_libraries(test_viewer_comic_workspace PRIVATE ra8_reader)
  target_compile_options(test_viewer_comic_workspace PRIVATE -Wall -Wextra -Werror)
  add_executable(
    test_viewer_output tests/src/test_viewer_output.c src/ra8_viewer_output.c ${RA8_STREAM_RAM_SRC}
                       ${RA8_STREAM_POSIX_SRC}
  )
  target_include_directories(
    test_viewer_output PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc ${FW_ROOT}/port/posix/inc
  )
  target_link_libraries(test_viewer_output PRIVATE ra8_reader)
  target_compile_options(test_viewer_output PRIVATE -Wall -Wextra -Werror)
  add_test(NAME viewer_workspace
           COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_workspace_test.sh
                   $<TARGET_FILE:test_viewer_workspace> ${CMAKE_CURRENT_BINARY_DIR}/workspace_test
  )
  add_test(NAME viewer_output COMMAND test_viewer_output)
  add_test(NAME viewer_comic_workspace COMMAND test_viewer_comic_workspace
                                               ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sample.cbz
  )
endif()

if(APPLE)
  set_source_files_properties(src/ra8_viewer_view.m PROPERTIES COMPILE_FLAGS "-fobjc-arc")
  target_link_libraries(rabook_viewer PRIVATE "-framework Cocoa" "-framework QuartzCore")
else()
  # The SVG rasteriser calls sinf/cosf/tanf/sqrtf/atan2f. macOS keeps those in
  # libSystem; every other POSIX host needs libm named explicitly.
  target_link_libraries(ra8_reader PUBLIC m)
  target_link_libraries(rabook_viewer PRIVATE m)
endif()
