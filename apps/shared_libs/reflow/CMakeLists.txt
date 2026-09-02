# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# apps/shared_libs/reflow -- HTML/CSS reflow + paginate engine for the ereader.
#
# Domain layer that takes XHTML (typically the per-chapter byte buffer
# returned by epub_load_chapter()) plus a viewport size and a TTF
# font, runs a greedy line-break + page-break pass, and rasterises the
# resulting pages into a caller-owned ra8_gfx framebuffer.
#
# Layout:
#   inc/reflow.h                public API
#   src/reflow_parse.c          parser entry point (resets pools, dispatches)
#   src/reflow_tokenize.c       no-heap streaming HTML-subset tokenizer
#   src/reflow_layout.c         line-break + page-break engine
#   src/reflow_render.c         glyph rasterise via stb_truetype
#
# Vendored dependencies (already at apps/shared_libs/third_party/):
#   stb_truetype.h     -- glyph rasteriser
#
# Like the other domain libs in this tree (ra8_fs, epub),
# consumers reach into inc/ + src/ directly. The host unit
# test build in tests/CMakeLists.txt already picks up libs that follow
# this convention and ships them through the ra8_core_hal OBJECT
# library; reflow is wired in next to epub there.
#
#

# =============================================================================
# v1 vs v2 (LiteHTML) selection
# =============================================================================
#
# The v1 engine (src/*.c) is the hand-rolled
# greedy line-break / page-break implementation that was validated
# against the EK-RA8D2 panel. v2 lives in v2/src/ and uses
# LiteHTML (vendored at apps/shared_libs/third_party/litehtml/) as the rendering
# engine. v2 is OFF by default until validated.
#
# Consumers (apps/board/stand_alone/ereader/, tests/) check this option and add
# either the v1 src/*.c globs or the v2/src/reflow_v2.cpp source.

option(REFLOW_USE_LITEHTML
       "Build the LiteHTML-backed v2 reflow engine instead of the v1 hand-rolled one" OFF
)

if(REFLOW_USE_LITEHTML)
  message(STATUS "reflow: using v2 (LiteHTML) engine")
else()
  message(STATUS "reflow: using v1 (hand-rolled) engine [default]")
endif()

# Consumers reach into inc/ + src/ (or v2/src/) directly.
