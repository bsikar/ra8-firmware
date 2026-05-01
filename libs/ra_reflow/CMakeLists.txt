#
# libs/ra_reflow -- HTML/CSS reflow + paginate engine for the ereader.
#
# Domain layer that takes XHTML (typically the per-chapter byte buffer
# returned by ra_epub_load_chapter()) plus a viewport size and a TTF
# font, runs a greedy line-break + page-break pass, and rasterises the
# resulting pages into a caller-owned ra_gfx framebuffer.
#
# Layout:
#   inc/ra_reflow.h                public API
#   src/ra_reflow_parse.c          DOM-walker (HTML subset -> token stream)
#   src/ra_reflow_layout.c         line-break + page-break engine
#   src/ra_reflow_render.c         glyph rasterise via stb_truetype
#   src/ra_reflow_xml_shim.cpp     C-callable wrapper around tinyxml2
#
# Vendored dependencies (already at libs/third_party/):
#   stb_truetype.h     -- glyph rasteriser
#   tinyxml2.h/.cpp    -- HTML/XML parser
#
# Like the other domain libs in this tree (ra_fs, ra_ble_host,
# ra_epub), consumers reach into inc/ + src/ directly. The host unit
# test build in tests/CMakeLists.txt already picks up libs that follow
# this convention and ships them through the ra_core_hal OBJECT
# library; ra_reflow is wired in next to ra_epub there.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

# Intentionally empty -- consumers reach into inc/ + src/ directly.
