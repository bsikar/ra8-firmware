#
# tools/ra8_viewer -- native macOS reader viewer (direct-call, not an emulator).
#
# Links the firmware's platform-agnostic reader libraries compiled host-side
# (RA8_SIMULATOR_MODE, C23) and calls them directly to rasterise e-book / comic
# pages into an RGB565 framebuffer, which a layer-backed Cocoa NSWindow presents.
# Unlike tools/board_sim (which emulates the firmware ELF under Unicorn), this
# tool runs no ARM code -- it is purely a viewer, so it is far faster.
#
#   cmake -B build -S . && cmake --build build -j
#   ./build/ra8_viewer path/to/book.cbz               # window; arrows turn pages
#   ./build/ra8_viewer book.cbz --headless --dump-ppm page0.ppm   # no display
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

project(ra8_viewer LANGUAGES C)
if(APPLE)
    enable_language(OBJC)
endif()

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

#Firmware root(two directories up from tools / ra8_viewer /).
get_filename_component(FW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# ---------------------------------------------------------------------------
# ra8_reader: static archive of exactly the firmware reader sources the viewer
# calls. Building an archive (not an object library) means the linker pulls only
# the members needed to resolve the viewer's references -- transitively -- so the
# HAL / RTOS / peripheral libraries are never dragged in. Under RA8_SIMULATOR_MODE
# the CBZ backend uses miniz's default (malloc) allocator, so no ra8_epub source
# is needed -- only its header (ra8_epub_miniz_alloc.h) sits on the include path.
# ---------------------------------------------------------------------------
file(GLOB RA8_COMIC_SRC  CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_comic/src/*.c)
file(GLOB RA8_GFX_SRC    CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_gfx/src/*.c)
# ra8_comic pulls in the gzip/xz/tar wrapped-container decoders (ra8_unarch),
# and ra8_unarch_xz sits on the vendored xz-embedded decoder.
file(GLOB RA8_UNARCH_SRC CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_unarch/src/*.c)

# ra8_longstrip renders the firmware-native `.jof` vertical-scroll tile atlas the
# downloader writes: the strip parses through ra8_jof (DEFLATE tiles inflate
# via ra8_io_compress over the already-linked miniz), pages through the ra8_mem
# tile cache (+ its keycache), and composites bands via ra8_longstrip. Only the
# READ path is needed (no producer / PNG / webp), so name the exact members.
set(RA8_LONGSTRIP_SRC
    ${FW_ROOT}/libs/ra8_longstrip/src/ra8_longstrip.c
    ${FW_ROOT}/libs/ra8_jof/src/ra8_jof.c
    ${FW_ROOT}/libs/ra8_io/src/ra8_io_compress.c
    ${FW_ROOT}/libs/ra8_mem/src/ra8_tile_cache.c
    ${FW_ROOT}/libs/ra8_mem/src/ra8_keycache.c
)

set(RA8_REFLOW_SRC
    ${FW_ROOT}/libs/ra8_reflow/src/ra8_reflow_image.c
    ${FW_ROOT}/libs/ra8_reflow/src/ra8_img_arena.c
    ${FW_ROOT}/libs/ra8_reflow/src/ra8_reflow_svg.c
    ${FW_ROOT}/libs/ra8_reflow/src/ra8_reflow_svg_doc.c
    ${FW_ROOT}/libs/ra8_reflow/src/ra8_reflow_svg_path.c
    ${FW_ROOT}/libs/ra8_reflow/src/ra8_reflow_svg_shape.c
    ${FW_ROOT}/libs/ra8_reflow/src/ra8_reflow_svg_xform.c
)
set(RA8_CORE_SRC
    ${FW_ROOT}/libs/ra8_core/src/ra8_log.c
    ${FW_ROOT}/libs/ra8_core/src/ra8_error_handler.c
    ${FW_ROOT}/libs/ra8_core/src/ra8_decomp_limits.c
)
file(GLOB RA8_XZ_EMBEDDED_SRC CONFIGURE_DEPENDS
    ${FW_ROOT}/libs/third_party/xz_embedded/*.c)
set(RA8_THIRD_PARTY_SRC
    ${FW_ROOT}/libs/third_party/miniz/miniz.c
    ${FW_ROOT}/libs/third_party/stb/stb_image_impl.c
    ${RA8_XZ_EMBEDDED_SRC}
)

add_library(ra8_reader STATIC
    ${RA8_COMIC_SRC}
    ${RA8_GFX_SRC}
    ${RA8_UNARCH_SRC}
    ${RA8_LONGSTRIP_SRC}
    ${RA8_REFLOW_SRC}
    ${RA8_CORE_SRC}
    ${RA8_THIRD_PARTY_SRC}
)
target_compile_definitions(ra8_reader PUBLIC RA8_SIMULATOR_MODE)
target_include_directories(ra8_reader PUBLIC
    ${FW_ROOT}/libs/ra8_core/inc
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
set_source_files_properties(${RA8_THIRD_PARTY_SRC} ${RA8_XZ_EMBEDDED_SRC}
    PROPERTIES COMPILE_OPTIONS "-w;-fno-strict-aliasing")

# ---------------------------------------------------------------------------
# ra8_viewer: the viewer executable -- C core + Cocoa window backend.
# ---------------------------------------------------------------------------
# The reader core is one module split across four TUs (see
# src/ra8_viewer_reader_internal.h): public API + dispatch, the comic engine, the
# JOF long-strip engine, and the shared RGB565 / PPM writer.
add_executable(ra8_viewer
    src/main.c
    src/ra8_viewer_reader.c
    src/ra8_viewer_comic.c
    src/ra8_viewer_jof.c
    src/ra8_viewer_ppm.c
    src/ra8_viewer_view.m
)
target_include_directories(ra8_viewer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc)
target_link_libraries(ra8_viewer PRIVATE ra8_reader)
target_compile_options(ra8_viewer PRIVATE -Wall -Wextra)

if(APPLE)
    set_source_files_properties(src/ra8_viewer_view.m PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    target_link_libraries(ra8_viewer PRIVATE "-framework Cocoa" "-framework QuartzCore")
endif()

