#
# libs/ra_box -- bounded box-model layout for e-reader chrome.
#
# A minimal, allocation-free, recursion-free CSS-style box model (vertical
# / horizontal stacks, fixed-column grid, padding / gap, fixed-vs-flex
# sizing) that lays a caller-owned node tree into positioned rectangles.
# Geometry only: no font, no framebuffer -- the renderer (ra_gfx) and
# hit-tester (ra_ui) layer on top. The document engine (ra_reflow) handles
# book content; ra_box handles the fixed application chrome (issue #80/#76).
#
# Layout:
#   inc/ra_box.h    public API (node, tree builder, layout)
#   src/ra_box.c    forward-pass layout engine
#
# Depends on ra_ui (shared ra_ui_rect_t geometry). Consumers reach into
# inc/ + src/ directly: firmware apps list `ra_box` (and `ra_ui`) in
# ra_add_app(LIBS ...); the host test build folds libs/ra_box/src/*.c into
# the ra_core_hal object library.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

# Header-and-source library; no standalone build target. See the note above.
