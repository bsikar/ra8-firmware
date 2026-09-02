# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# apps/shared_libs/epub -- EPUB (.epub) reader and chapter iterator.
#
# Domain layer that opens .epub files via miniz (ZIP container) and
# the bounded first-party XML pull reader, exposes a clean chapter
# iteration API, and forwards glyph rasterisation to stb_truetype.
#
# Layout:
#   inc/epub.h                public API
#   src/epub_open.c           open / close + miniz plumbing
#   src/epub_chapter.c        chapter, metadata, cover, glyph
#   src/epub_xml_shim.c       strict streamed XML consumers
#   src/epub_xml_toc.c        NCX and nav table-of-contents consumers
#
# Vendored dependencies (already at apps/shared_libs/third_party/):
#   stb_truetype.h, stb_image.h  -- glyph rasteriser + image decoder
#   miniz.h, miniz.c             -- ZIP / DEFLATE
#   apps/shared_libs/xml                 -- bounded XML pull reader
#
# Like the other domain libraries in this tree (ra8_fs, reflow),
# consumers reach into inc/ + src/ directly. The host unit test build
# in tests/CMakeLists.txt picks up epub via the recursive glob plus
# the third-party additions wired up there. When an example app starts
# to depend on epub, it does NOT need anything added here: name the
# library in the app's ra8_add_app(... LIBS epub), which
# cmake/ra8_app/sources.cmake resolves from the platform and app-owned shared
# library roots, then attaches the registered miniz vendor sources.
#
#

# Intentionally empty -- consumers reach into inc/ + src/ directly.
