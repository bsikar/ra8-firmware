# ereader_manga -- #231 full-resolution image path, end to end

Headless gate for the #231 producer chain: an all-image EPUB's PNG page is
transcoded **at import time** into the RTA1 band-tile atlas
(`libs/ra8_tileatlas`) and then rendered **decode-on-demand** through the
EPUB tile binder (`ra8_epub_img_tiles`) -- full resolution, no downscaling,
whole decoded image never resident.

## What it does

1. Opens the baked 15 KB fixture EPUB (`manga_hil_fixture.h`) through the
   production streamed open (`ra8_epub_open_streamed`).
2. `ra8_epub_tile_binder_import("page1.png")` streams the DEFLATE-compressed
   PNG entry through the bounded transcode producer
   (`ra8_tileatlas_produce`): the 512x512 grayscale page (256 KiB decoded)
   flows one 64 KiB band at a time into 16 deflate-coded 128x128 tiles in an
   SDRAM memstore. The producer's whole working set is one fixed SDRAM
   arena; the decoded page is never resident.
3. Renders a 160x120 centre crop by paging the 4 covering tiles through a
   deliberately tiny **2-cell** tile cache -- decode-on-demand with forced
   evictions -- expanding gray8 to RGB565.
4. Prints the atlas geometry, atlas byte count and an FNV-1a-32 framebuffer
   hash over the SCI8 J-Link OB console:

   `ereader-manga-hil: import 512x512 tiles=16 atlas=<N> crc=<8hex> ok`

Every stage is a deterministic integer pipeline, so the banner is identical
on the host twin (`tests/test_app_ereader_manga.c`), board_sim, and silicon.

## Fixture provenance

`manga_hil_fixture.h` is generated: a Python `zipfile` EPUB (stored
`mimetype` first, everything else `ZIP_DEFLATED`) whose single page is a
Python-built 512x512 gray8 PNG (all rows filter 0, IDAT `zlib.compress(...,
6)`). The pixel at `(x, y)` is `(uint8_t)((x ^ y) + ((x + y) >> 2))` -- the
host twin regenerates the same pattern independently for byte-parity
assertions, so fixture and test cannot drift apart silently.

## Status

Sim-gated (`hw_pending`): the pipeline is pure computation + UART, fully
modelled by board_sim; not yet run on a bench board.

## Build / run

```
make ereader_manga                      # from the repo root
tools/board_sim/build/board_sim examples/ek_ra8d2/hw_pending/ereader_manga/build/ereader_manga.elf
```
