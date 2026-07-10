#
# libs/ra_comic -- comic-book-archive reader (CBZ = ZIP of images, CBR = RAR of images).
#
# Domain layer that opens a `.cbz` (via the streaming miniz ZIP reader, the same
# path `ra_epub_open_streamed` uses) or a `.cbr` (via the first-party clean-room
# RAR walker `ra_rar`), builds a name-sorted page index, and serves each page's
# encoded image bytes on demand for `ra_reflow`'s `ra_img_decode_blit`.
#
# Layout:
#   inc/ra_comic.h            public facade (open / page index / page read)
#   inc/ra_rar.h              clean-room RAR4/RAR5 reader (STORE extraction)
#   src/ra_comic.c            magic detect, page-index sort, dispatch
#   src/ra_comic_cbz.c        CBZ backend (streaming miniz ZIP reader)
#   src/ra_comic_cbr.c        CBR backend (RAR walk -> page index)
#   src/ra_rar.c              RAR walker implementation
#
# Dependency note: the CBZ backend needs miniz built WITH its archive APIs and,
# on firmware, the zero-heap `ra_epub_miniz_alloc` static pool. Both are supplied
# when an app also lists `ra_epub` in `LIBS` (see cmake/ra_add_app.cmake, which
# compiles miniz.c with the ZIP reader for the ra_epub path). A comic app should
# therefore pull `LIBS ... ra_comic ra_epub`.
#
# Licensing: `ra_rar` is a first-party clean-room RAR *reader* written from the
# public RAR format note. It vendors no `unrar` code and carries no SBOM entry;
# see the header for the rationale.
#
# Like the other domain libs in this tree, consumers reach into inc/ + src/
# directly; app builds glob libs/ra_comic/src/*.c via ra_add_app(LIBS ra_comic ...).
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

# Intentionally empty -- consumers reach into inc/ + src/ directly.
