# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# apps/shared_libs/longstrip -- continuous vertical-scroll (longstrip / manhwa) reading
# mode (#289). A thin scroll/geometry engine over the JOF band-tile atlas
# (#231, apps/shared_libs/jof): a longstrip band is one full-width JOF tile
# column, so the tile index IS the band index -- O(1) seek to any scroll-y.
#
# Layout:
#   inc/longstrip.h   public API: open, geometry (y->band), scroll + fling
#                       state machine, bounded prefetch, visible-band composite
#   src/longstrip.c   pure-integer implementation (no MMIO -- runs identically
#                       on target, ra8_emulator and the unit-test host)
#
# Dependencies: ra8_core (err/check/log), jof (parse + per-band
# decode), ra8_mem (ra8_tile_cache band paging). The composite is a DIP blit
# seam, so longstrip does NOT depend on ra8_gfx/ra8_drw; the caller binds the
# software (ra8_gfx_blit) or DRW-accelerated blitter.
#
# Like the other domain libs in this tree, consumers reach into inc/ + src/
# directly: the host unit-test build globs it in tests/CMakeLists.txt and apps
# list `longstrip` in their ra8_add_app(... LIBS ...).
#
#

# Intentionally empty -- consumers reach into inc/ + src/ directly.
