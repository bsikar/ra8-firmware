# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# libs/ra8_ui -- bounded UI interaction core for the e-reader.
#
# The interaction / controller layer that the rendering engines
# (ra8_gfx, ra8_reflow) do not provide: hit-testing of tap targets,
# a fixed-depth screen-id navigation stack, and a clamped page cursor.
# Allocation-free plain-data logic, identical on host and target.
#
# Layout:
#   inc/ra8_ui.h     public API
#   src/ra8_ui.c     hit-test + screen stack + paging
#
# Like the other domain libs in this tree (ra8_reflow, ra8_epub, ra8_fs),
# consumers reach into inc/ + src/ directly: firmware apps list `ra8_ui`
# in ra8_add_app(LIBS ...) and the host unit-test build folds
# libs/ra8_ui/src/*.c into the ra8_core_hal object library.
#
#

# Header-and-source library; no standalone build target. See the note above.
