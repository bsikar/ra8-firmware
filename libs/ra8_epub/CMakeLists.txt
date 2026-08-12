# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# libs/ra8_epub -- EPUB (.epub) reader and chapter iterator.
#
# Domain layer that opens .epub files via miniz (ZIP container) and
# tinyxml2 (OPF + container.xml parser), exposes a clean chapter
# iteration API, and forwards glyph rasterisation to stb_truetype.
#
# Layout:
#   inc/ra8_epub.h                public API
#   src/ra8_epub_open.c           open / close + miniz plumbing
#   src/ra8_epub_chapter.c        chapter, metadata, cover, glyph
#   src/ra8_epub_xml_shim.cpp     C-callable wrapper around tinyxml2
#
# Vendored dependencies (already at libs/third_party/):
#   stb_truetype.h, stb_image.h  -- glyph rasteriser + image decoder
#   miniz.h, miniz.c             -- ZIP / DEFLATE
#   tinyxml2.h, tinyxml2.cpp     -- XML parser
#
# Like the other domain libs in this tree (ra8_fs, ra8_reflow),
# consumers reach into inc/ + src/ directly. The host unit test build
# in tests/CMakeLists.txt picks up ra8_epub via the recursive glob plus
# the third-party additions wired up there. When an example app starts
# to depend on ra8_epub, it does NOT need anything added here: name the
# library in the app's ra8_add_app(... LIBS ra8_epub), which
# cmake/ra8_app/sources.cmake resolves by globbing libs/<lib>/src/*.c and
# appending libs/<lib>/inc. That is how every library consumer in this tree
# actually links, including ra8_tls.
#
#

# Intentionally empty -- consumers reach into inc/ + src/ directly.
