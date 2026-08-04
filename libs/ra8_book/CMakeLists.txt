# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# libs/ra8_book -- flat, execute-in-place container for a compiled e-book.
#
# The on-device representation of a book that has already been unzipped,
# XML-parsed and image-transcoded on the host by tools/epub_compile. The
# firmware never unzips or parses XHTML at runtime: it points a
# `const ra8_book_header_t*` at a `.rabook` blob (which may live in
# memory-mapped Octo-SPI flash) and walks a faithful, pre-parsed DOM with
# the inline accessors in the header. Every internal reference is a
# blob-relative byte offset, so the structure is position-independent and
# safe to read straight from flash with no relocation and no copy to RAM.
#
# Fidelity is the rule: every element keeps its real tag name and full
# attribute list and every stylesheet is preserved verbatim. Markup the
# renderer cannot lay out yet is still present intact -- the fix for
# unsupported content is to grow ra8_reflow, never to strip the book. The
# only transform is raster images, which the host transcodes to the panel's
# native 4bpp grayscale at full source resolution.
#
# Layout:
#   inc/ra8_book.h   format structs + header-only XIP accessors
#   src/ra8_book.c   the one non-inline part: blob integrity/bounds validation
#
# Depends only on ra8_core (ra8_err, ra8_check, ra8_attributes). Consumers reach
# into inc/ + src/ directly: firmware apps list `ra8_book` in
# ra8_add_app(LIBS ...); the host test build folds libs/ra8_book/src/*.c into
# the ra8_core_hal object library (see tests/CMakeLists.txt).
#
#

# Header-and-source library; no standalone build target. See the note above.
