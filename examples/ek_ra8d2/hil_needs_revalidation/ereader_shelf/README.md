# ereader_shelf

A full e-reader GUI on the EK-RA8D2 7.0-inch 1024x600 parallel TFT, and the
visible front end of the compiled-book (`.rabook`) pipeline: a bookshelf of
cover art, a cover/title page, a table of contents, and a reader that paginates
every chapter of a whole book. Page turns cross chapter boundaries; tapping the
header steps back up through reader -> TOC -> cover -> shelf.

Books come from two sources side by side in one app: a few compiled into MRAM,
and the rest discovered on a FAT card at boot. A card book may be a
pre-compiled `.rabook`, a plain `.epub` parsed on-device by `ra8_epub`, or a
comic archive (`.cbz` / `.cbr` / `.cbt`) opened by `ra8_comic` (#236).
`sh_book.c` hides the difference behind a uniform chapter-text / TOC-label /
cover API, so every screen is identical across all three sources; EPUB chapters
arrive as XHTML and are stripped to the same plain text the reader word-wraps.

Since `ra8_fs` gained VFAT long-name write (#600) the shelf keeps each source's
own name verbatim (#633), and the classifier still accepts the legacy 8.3
truncations so cards written by the old tools keep resolving.

## Everything is demand-paged, deliberately

`.rabook` books are always demand-paged (#204/#205): the chunked RBKC reader
backs an `ra8_vmem` page cache and single chunks inflate into cache frames as
they are touched. There is intentionally no resident-versus-paged size
threshold -- a small book never evicts anything from the SDRAM frame pool, so it
behaves exactly like the old resident fast path, while a book larger than the
pool evicts normally through the same code. One code path, no cliff. The
chunk-table budget caps an openable book at 64 MiB inflated.

Boot paints the shelf from pre-baked grayscale cover thumbnails embedded
alongside the baked books, so it never has to open a book just to draw the
shelf, and SD cover art loads lazily on first open. Opening a book costs only
the chunks it actually touches -- header, metadata, cover, current chapter --
never a whole-book inflate.

## Comics carry no reading direction

A raw CBZ or CBR is just a container of page images in reading order, with no
metadata saying which way to read them. Right-to-left (manga) is therefore an
app-level toggle on the comic screen's header, and flipping it mirrors the
edge-tap zones so the left edge advances. `ra8_comic` streams one page's encoded
image at a time and the page is re-decoded on each turn through the same integer
decode pipeline the cover uses, so a page has to fit the bounded decode buffer;
tile-cache streaming for large manga pages is #231/#232.

Holding SW1 at boot runs a self-demo that walks every screen, and the first
touch takes over. Without SW1 the app boots to the shelf and idles rather than
looping, which keeps it responsive.

Needs a microSD for the card half; an unseated card leaves only the baked books,
and is the first thing to rule out when the shelf comes up short.
