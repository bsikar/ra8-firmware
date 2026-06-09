#
# libs/ra_ui -- bounded UI interaction core for the e-reader.
#
# The interaction / controller layer that the rendering engines
# (ra_gfx, ra_reflow) do not provide: hit-testing of tap targets,
# a fixed-depth screen-id navigation stack, and a clamped page cursor.
# Allocation-free plain-data logic, identical on host and target.
#
# Layout:
#   inc/ra_ui.h     public API
#   src/ra_ui.c     hit-test + screen stack + paging
#
# Like the other domain libs in this tree (ra_reflow, ra_epub, ra_fs),
# consumers reach into inc/ + src/ directly: firmware apps list `ra_ui`
# in ra_add_app(LIBS ...) and the host unit-test build folds
# libs/ra_ui/src/*.c into the ra_core_hal object library.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

# Header-and-source library; no standalone build target. See the note above.
