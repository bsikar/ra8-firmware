# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# libs/ra8_zoom -- the reader's tap-to-zoom image viewer (#478).
#
# One viewport engine behind a gray8 sub-rectangle seam, serving both
# presentations the reader needs: full-screen zoom (a tapped EPUB figure or
# comic page fills the panel) and a loupe (a small lens composited over the
# page, so a drag costs one small partial e-ink update instead of a full
# refresh). The magnification ladder is integer, the viewport never holds the
# visible window -- it composites in bounded horizontal strips -- and every
# buffer is caller-supplied, so the module allocates nothing.
#
# Layout:
#   inc/ra8_zoom.h        public API: source seam, viewport state machine,
#                         pan / zoom-at-point, dirty rect + waveform plan
#   inc/ra8_zoom_book.h   adapter: a `.rabook` image-pool figure as a source
#   inc/ra8_zoom_tiles.h  adapter: a tiled atlas paged by an ra8_tile_cache
#   src/ra8_zoom.c        viewport state machine (pure integer, no MMIO)
#   src/ra8_zoom_render.c strip composite: magnify -> re-dither -> blit
#   src/ra8_zoom_internal.h  the shared axis geometry both of the above use
#   src/ra8_zoom_book.c   thin wrapper over ra8_book_src_image_rect
#   src/ra8_zoom_tiles.c  pixel rect -> tiles -> acquire / copy / release
#
# Dependencies: ra8_core (err/check/log), ra8_ui (the rect type), and ra8_gfx
# for the render TU -- the blue-noise dither (#477) IS the feature here, so
# unlike ra8_longstrip this library does not push the blit behind a seam. The
# two adapter TUs are each guarded on `__has_include` of their backend header
# and compile to nothing otherwise, so an app that magnifies only `.rabook`
# figures never links ra8_mem, and one that magnifies only comics never links
# ra8_book.
#
# Like the other domain libs in this tree, consumers reach into inc/ + src/
# directly: the host unit-test build globs it in tests/cmake/, and apps list
# `ra8_zoom` in their ra8_add_app(... LIBS ...).
#
#

# Intentionally empty -- consumers reach into inc/ + src/ directly.
