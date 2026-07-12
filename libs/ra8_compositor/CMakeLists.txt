#
# libs/ra8_compositor -- zero-heap framebuffer compositor for the widget UI.
#
# The framebuffer half of issue #145: a fixed-pool widget tree (container /
# rect / label) with a dwm-style tiling layout pass and a clipped, dirty-region
# compositor that paints into a caller-owned framebuffer through ra8_gfx (used
# read-only). The sibling ra8_widget library models the pure layout / route /
# damage selection logic; this one closes the gap by actually compositing
# pixels and proving the partial-update invariant (dirty recomposite is
# byte-identical to a full recomposite) on the host.
#
# Layout:
#   inc/ra8_compositor.h   public API (widget node, scene, layout, compositor)
#   src/ra8_compositor.c   tiling layout + clipped paint + damage union
#
# Like the other domain libs in this tree (ra8_ui, ra8_widget, ra8_reflow),
# ra8_compositor is a header-and-source library with no standalone build
# target: firmware apps list `ra8_compositor` in ra8_add_app(LIBS ...) and the
# host unit-test build folds libs/ra8_compositor/src/*.c into the ra8_core_hal
# object library. Its only dependencies are ra8_core (errors / checks) and
# ra8_gfx (the pixel backend).
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

# Header-and-source library; no standalone build target. See the note above.
