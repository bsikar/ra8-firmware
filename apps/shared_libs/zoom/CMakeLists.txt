# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# apps/shared_libs/zoom -- the reader's tap-to-zoom image viewer.
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
#   inc/zoom.h        public API: source seam, viewport state machine,
#                         pan / zoom-at-point, dirty rect + waveform plan
#   inc/zoom_book.h   adapter: a `.rabook` image-pool figure as a source
#   inc/zoom_tiles.h  adapter: a tiled atlas paged by an ra8_tile_cache
#   (src/zoom.c is gone: the viewport state machine is Zig, RA8FW-1256)
#   (src/zoom_render.c is gone: the strip composite is Zig, RA8FW-1260;
#   src/zoom_render.zig holds magnify -> re-dither -> blit)
#   src/zoom_internal.h  the shared axis geometry both of the above use
#   (src/zoom_book.c is gone: the .rabook source adapter is Zig, RA8FW-1259)
#   src/zoom_tiles.c  pixel rect -> tiles -> acquire / copy / release
#
# Dependencies: ra8_core (err/check/log), ra8_ui (the rect type), and ra8_gfx
# for the render TU -- the blue-noise dither IS the feature here, so
# unlike longstrip this library does not push the blit behind a seam. The
# two adapter TUs are each guarded on `__has_include` of their backend header
# and compile to nothing otherwise, so an app that magnifies only `.rabook`
# figures never links ra8_mem, and one that magnifies only comics never links
# book.
#
# Like the other domain libs in this tree, consumers reach into inc/ + src/
# directly: the host unit-test build globs it in tests/cmake/, and apps list
# `zoom` in their ra8_add_app(... LIBS ...).
#
#

# Intentionally empty -- consumers reach into inc/ + src/ directly.
