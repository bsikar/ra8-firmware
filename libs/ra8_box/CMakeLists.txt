# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# libs/ra8_box -- bounded box-model layout for e-reader chrome.
#
# A minimal, allocation-free, recursion-free CSS-style box model (vertical
# / horizontal stacks, fixed-column grid, padding / gap, fixed-vs-flex
# sizing) that lays a caller-owned node tree into positioned rectangles.
# Geometry only: no font, no framebuffer -- the renderer (ra8_gfx) and
# hit-tester (ra8_ui) layer on top. The document engine (ra8_reflow) handles
# book content; ra8_box handles the fixed application chrome (issue #80/#76).
#
# Layout:
#   inc/ra8_box.h    public API (node, tree builder, layout)
#   src/ra8_box.c    forward-pass layout engine
#
# Depends on ra8_ui (shared ra8_ui_rect_t geometry). Consumers reach into
# inc/ + src/ directly: firmware apps list `ra8_box` (and `ra8_ui`) in
# ra8_add_app(LIBS ...); the host test build folds libs/ra8_box/src/*.c into
# the ra8_core_hal object library.
#
#

# Header-and-source library; no standalone build target. See the note above.
