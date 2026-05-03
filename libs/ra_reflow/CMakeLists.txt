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

# =============================================================================
# v1 vs v2 (LiteHTML) selection
# =============================================================================
#
# The v1 engine (src/*.c + ra_reflow_xml_shim.cpp) is the hand-rolled
# greedy line-break / page-break implementation that was validated
# against the EK-RA8D2 panel. v2 lives in v2/ and uses
# LiteHTML (vendored at libs/third_party/litehtml/) as the rendering
# engine. v2 is OFF by default until validated.
#
# Consumers (examples/ereader/, tests/) check this option and add
# either the v1 src/*.c globs or the v2/ra_reflow_v2.cpp source.

option(RA_REFLOW_USE_LITEHTML
  "Build the LiteHTML-backed v2 ra_reflow engine instead of the v1 hand-rolled one"
  OFF
)

if(RA_REFLOW_USE_LITEHTML)
    message(STATUS "ra_reflow: using v2 (LiteHTML) engine")
else()
    message(STATUS "ra_reflow: using v1 (hand-rolled) engine [default]")
endif()

# Consumers reach into inc/ + src/ (or v2/) directly.

