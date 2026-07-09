# ereader_shelf -- hybrid baked + SD e-reader

A full e-reader GUI on the EK-RA8D2 7.0-inch 1024x600 parallel TFT. It is the
visible front end of the compiled-book (`.rabook`) pipeline: a bookshelf of
cover art, a cover/title page, a table of contents, and a reader that paginates
**every chapter of the whole book**.

Books come from **two sources, tested side by side in one app**:

- **Baked** -- a few books compiled to `.rabook` and embedded in MRAM
  (`library.h`, the chunked RBKC container).
- **SD card** -- the rest live on a FAT card, discovered at boot and opened on
  tap, in **either** format:
  - `BOOKnn.RBK` -- a pre-compiled `.rabook`, read chunk-by-chunk from the file.
  - `BOOKnn.EPB` -- a plain **`.epub`**, parsed on-device by `ra_epub` (ZIP +
    XML) so you can drop an ordinary book on the card without pre-compiling.
    (`.epub` truncates to the 3-char 8.3 extension `.EPB` on FAT.)

`.rabook` books are **always demand-paged** (#204/#205): `sh_paged.c` binds the
chunked RBKC reader (`ra_book_chunked`) as the backing of an `ra_vmem` page
cache and single 64 KiB chunks inflate into cache frames as they are touched --
the whole book is never inflated, from MRAM or from SD. There is deliberately
no resident/paged size threshold: a small book never evicts anything from the
24 MiB SDRAM frame pool (so it behaves exactly like the old resident fast
path), while a book larger than the pool evicts normally through the same code.
The chunk-table budget caps an openable book at 64 MiB inflated (1024 chunks).

A small `sh_book.c` backend hides the difference: it opens the right parser by
format and exposes a uniform chapter-text / TOC-label / cover API, so the
shelf/cover/TOC/reader screens are identical across all three sources. EPUB
chapters arrive as XHTML and are stripped to the same plain text the reader
word-wraps; EPUB covers (JPEG/PNG) decode via `ra_img_decode_blit`.

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

Hold **SW1 at boot** (`board_sim --button 1`) to run a self-demo that walks every
screen -- handy for a headless `board_sim --record`. The first touch takes over.
Without SW1 the app just boots to the shelf and idles, so interactive use (and
board_sim, which fast-forwards an idle core) stays responsive instead of grinding
through a continuous re-open loop.

### Performance notes

Boot draws the shelf from **pre-baked gray8 cover thumbnails** embedded in
`library.h` (decoded once by `tools/bake_library.py`), so it never touches a
book just to paint the shelf. Under board_sim (a ~125x-slower instruction
emulator), this cut cold boot from ~17 s to ~2 s; on the real 1 GHz device both
are sub-second. Opening a book costs only the chunk faults it actually touches
(header, metadata, cover, current chapter) -- never a whole-book inflate --
instant on hardware, a couple seconds in the emulator.

## Modules (`src/`)

| File          | Role                                                            |
|---------------|-----------------------------------------------------------------|
| `main.c`      | boot, chunk-inflate callback, input loop, screen dispatch      |
| `sh_paged.c`  | demand-paged book bind: chunk reader + `ra_vmem` frame cache    |
| `sh_image.c`  | 4bpp-grayscale cover/image decode + aspect-fit scaled blit      |
| `sh_shelf.c`  | cover-card grid (`ra_box`), boot thumbnail cache                |
| `sh_cover.c`  | cover / title page                                              |
| `sh_toc.c`    | scrollable chapter list                                         |
| `sh_reader.c` | per-chapter text extract, UTF-8->ASCII fold, word-wrap, paginate|
| `sh_book.c`   | format backend: ra_book vs ra_epub dispatch + XHTML->text strip |
| `sh_sd.c`     | Pmod2 microSD bring-up (`ra_sdmmc_spi` -> `ra_fs`), scan, paged reads |
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
  "content/library/Pride and Prejudice - Jane Austen.epub"   # .rabook or .epub
tools/board_sim/build/board_sim .../ereader_shelf.elf --sd books.img --ppm shelf.ppm
```

The baked set is regenerated with `tools/bake_library.py` (see the head of
`library.h`).

## Caveat: board_sim SPI speed (and `--fast-sd`)

board_sim emulates the SD-over-SPI bus byte by byte, so even the demand-paged
chunk reads behind an open (header, chunk table, cover chunks) take far longer
than a real card (which runs at 25 MHz and reads a chunk in ~ms). Mounting,
directory scan, and the shelf's SD placeholders are board_sim-validated; the
full seek -> chunk-inflate -> render path is proven in board_sim and validated
at speed on the same `ra_fs` / `ra_sdmmc_spi` stack used by `pagecache` and
`epub_open`. To keep boot instant, SD cover art loads lazily on first open
rather than at boot.

To open a **full-size** book at speed in board_sim, pass the opt-in `--fast-sd`
flag: it serves whole 512-byte blocks straight from the card image
(`board_sd_read_block`) in one step instead of clocking each byte through five
MMIO callbacks, so a book whose chunk faults otherwise never finish opens in
one window. The firmware receives identical bytes, so the rendered cover is
**byte-for-byte identical** with and without the flag; only the per-byte SPI
plumbing is shortcut (still validated by `tests/test_ra_sdmmc_card_reflow.c` and
on hardware). It is off by default so the HIL gate exercises the full handshake.
The SD read is then effectively instant; the residual cost for a large cover is
the firmware's own per-pixel decode (a 683x1024 cover is ~700 K pixels),
emulated at board_sim's ~125x compute slowdown -- sub-second on the real device.

```sh
tools/board_sim/build/board_sim .../ereader_shelf.elf \
  --sd oz.img --click 887 210 --fast-sd --ppm oz.ppm   # full Oz, fast
```

`BOARD_SIM_CLICK_SETTLE=N` widens the post-`--click` drain when a tap kicks off a
long load.

## HIL gate

`hil.conf` runs the baked-only configuration under `board_sim` and matches a
banner that digests the rendered framebuffer
(`ereader-shelf: books=N sd=B fb=HASH ok`), so a cover-decode or layout
regression -- not just a boot failure -- trips the gate.
