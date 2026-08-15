# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# libs/ra8_rabook_compile -- zero-heap RABOOK1 emitter (the #149 compiler back-end).
#
# Takes an in-memory book model (a DOM of element/text nodes with attributes,
# spine chapters, a string pool, transcoded images and preserved stylesheets,
# plus metadata) and serializes the exact binary RABOOK1 blob that the desktop
# tool tools/epub_compile/epub_compile.py emits and the on-device reader
# libs/ra8_book parses. This is ONLY the emitter; the XHTML -> DOM front-end and
# the raster -> gray4 image transcode are the next stage-(a) pieces (#149).
#
# No dynamic allocation (NASA Power-of-10 Rule 3): the caller hands the builder
# fixed, caller-sized arenas and the output buffer. Depends only on ra8_book (the
# format structs) and ra8_core (ra8_err, ra8_check). Consumers reach into inc/ +
# src/ directly: firmware apps list `ra8_rabook_compile` in ra8_add_app(LIBS ...);
# the host test build folds libs/ra8_rabook_compile/src/*.c into the ra8_core_hal
# object library (see tests/CMakeLists.txt).
#
# Layout:
#   inc/ra8_rabook_compile.h   the zero-heap builder API
#   src/ra8_rabook_compile.c   mutable zero-heap builder operations
#   src/ra8_rabook_finalize.c  canonical layout, CRC-32, stream publication
#
#

# Header-and-source library; no standalone build target. See the note above.
