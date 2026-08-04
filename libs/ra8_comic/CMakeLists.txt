# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# libs/ra8_comic -- comic-book-archive reader (CBZ = ZIP of images, CBR = RAR of images).
#
# Domain layer that opens a `.cbz` (via the streaming miniz ZIP reader, the same
# path `ra8_epub_open_streamed` uses) or a `.cbr` (via the first-party clean-room
# RAR walker `ra8_rar`), builds a name-sorted page index, and serves each page's
# encoded image bytes on demand for `ra8_reflow`'s `ra8_img_decode_blit`.
#
# Layout:
#   inc/ra8_comic.h            public facade (open / page index / page read)
#   inc/ra8_rar.h              clean-room RAR4/RAR5 reader (STORE extraction)
#   src/ra8_comic.c            magic detect, page-index sort, dispatch
#   src/ra8_comic_cbz.c        CBZ backend (streaming miniz ZIP reader)
#   src/ra8_comic_cbr.c        CBR backend (RAR walk -> page index)
#   src/ra8_rar.c              RAR walker implementation
#
# Dependency note: the CBZ backend needs miniz built WITH its archive APIs and,
# on firmware, the zero-heap `ra8_epub_miniz_alloc` static pool. Both are supplied
# when an app also lists `ra8_epub` in `LIBS` (see cmake/ra8_add_app.cmake, which
# compiles miniz.c with the ZIP reader for the ra8_epub path). A comic app should
# therefore pull `LIBS ... ra8_comic ra8_epub`.
#
# Licensing: `ra8_rar` is a first-party clean-room RAR *reader* written from the
# public RAR format note. It vendors no `unrar` code and carries no SBOM entry;
# see the header for the rationale.
#
# Like the other domain libs in this tree, consumers reach into inc/ + src/
# directly; app builds glob libs/ra8_comic/src/*.c via ra8_add_app(LIBS ra8_comic ...).
#
#

# Intentionally empty -- consumers reach into inc/ + src/ directly.
