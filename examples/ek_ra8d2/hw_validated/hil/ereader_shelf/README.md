# ereader_shelf -- hybrid baked + SD e-reader

A full e-reader GUI on the EK-RA8D2 7.0-inch 1024x600 parallel TFT. It is the
visible front end of the compiled-book (`.rabook`) pipeline: a bookshelf of
cover art, a cover/title page, a table of contents, and a reader that paginates
**every chapter of the whole book**.

Books come from **two sources, tested side by side in one app**:

- **Baked** -- a few books compiled to `.rabook` and embedded in MRAM
  (`library.h`, the compressed RBKZ container). Inflated into SDRAM on demand.
- **SD card** -- the rest live as `BOOKnn.RBK` files on a FAT card, discovered at
  boot and read on tap.

Either way the bytes are inflated by `ra_book_open()` (heap-free `tinfl`) and
rendered by the same source-agnostic screens, so the load path is the only
difference.

## Screens

```
shelf (cover-thumbnail grid)
  -> tap a book -> cover / title page (full cover, Read / Contents)
       -> Contents -> table of contents (real chapter labels, scrollable)
            -> tap a chapter -> reader
       -> Read -> reader (first prose chapter)
  reader: tap left/right third or SW1/SW2 to turn pages -- page turns cross
          chapter boundaries; the header shows "Ch c/N  p/q". Tap the header
          to step back up (reader -> TOC -> cover -> shelf).
```

With no input, an idle self-demo walks the screens (so a headless `board_sim
--record` captures the whole flow); the first touch takes over.

## Modules (`src/`)

| File          | Role                                                            |
|---------------|-----------------------------------------------------------------|
| `main.c`      | boot, inflate callback, source resolution, input loop, dispatch |
| `sh_image.c`  | 4bpp-grayscale cover/image decode + aspect-fit scaled blit      |
| `sh_shelf.c`  | cover-card grid (`ra_box`), boot thumbnail cache                |
| `sh_cover.c`  | cover / title page                                              |
| `sh_toc.c`    | scrollable chapter list                                         |
| `sh_reader.c` | per-chapter text extract, UTF-8->ASCII fold, word-wrap, paginate|
| `sh_sd.c`     | Pmod2 microSD bring-up (`ra_sdmmc_spi` -> `ra_fs`), scan, read   |
| `sh_util.c`   | shared draw/format helpers                                      |

## Build + run

```sh
make ereader_shelf                       # build

# baked-only (3 MRAM books with cover art):
tools/board_sim/build/board_sim \
  examples/ek_ra8d2/hw_validated/hil/ereader_shelf/build/ereader_shelf.elf \
  --ppm shelf.ppm

# hybrid baked + SD: build a FAT image of extra books, attach with --sd:
cmake -S tools/mkbookimg -B tools/mkbookimg/build && cmake --build tools/mkbookimg/build
tools/mkbookimg/build/mkbookimg books.img \
  "content/compiled/The Wonderful Wizard of Oz - L Frank Baum.rabook" \
  "content/compiled/Pride and Prejudice - Jane Austen.rabook"
tools/board_sim/build/board_sim .../ereader_shelf.elf --sd books.img --ppm shelf.ppm
```

The baked set is regenerated with `tools/bake_library.py` (see the head of
`library.h`).

## Caveat: board_sim SPI speed

board_sim emulates the SD-over-SPI bus byte by byte, so reading a *book-sized*
file (200-450 KB) takes far longer than a real card (which runs at 25 MHz and
reads a book in ~0.1 s). Mounting, directory scan, and the shelf's SD
placeholders are board_sim-validated; the full read -> inflate -> render path is
proven in board_sim with a small fixture book and validated at speed on the same
`ra_fs` / `ra_sdmmc_spi` stack used by `pagecache` and `epub_open`. To keep boot
instant, SD cover art loads lazily on first open rather than at boot.

## HIL gate

`hil.conf` runs the baked-only configuration under `board_sim` and matches a
banner that digests the rendered framebuffer
(`ereader-shelf: books=N sd=B fb=HASH ok`), so a cover-decode or layout
regression -- not just a boot failure -- trips the gate.
