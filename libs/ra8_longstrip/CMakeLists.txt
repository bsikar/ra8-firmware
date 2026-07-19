#
# libs/ra8_longstrip -- continuous vertical-scroll (longstrip / manhwa) reading
# mode (#289). A thin scroll/geometry engine over the JOF band-tile atlas
# (#231, libs/ra8_tileatlas): a longstrip band is one full-width JOF tile
# column, so the tile index IS the band index -- O(1) seek to any scroll-y.
#
# Layout:
#   inc/ra8_longstrip.h   public API: open, geometry (y->band), scroll + fling
#                       state machine, bounded prefetch, visible-band composite
#   src/ra8_longstrip.c   pure-integer implementation (no MMIO -- runs identically
#                       on target, board_sim and the unit-test host)
#
# Dependencies: ra8_core (err/check/log), ra8_tileatlas (parse + per-band
# decode), ra8_mem (ra8_tile_cache band paging). The composite is a DIP blit
# seam, so ra8_longstrip does NOT depend on ra8_gfx/ra8_drw; the caller binds the
# software (ra8_gfx_blit) or DRW-accelerated blitter.
#
# Like the other domain libs in this tree, consumers reach into inc/ + src/
# directly: the host unit-test build globs it in tests/CMakeLists.txt and apps
# list `ra8_longstrip` in their ra8_add_app(... LIBS ...).
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

# Intentionally empty -- consumers reach into inc/ + src/ directly.
