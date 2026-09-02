# ereader_manga

A viewable reader for a page far larger than the 1024x600 panel. The baked page
is transcoded through `jof_produce` into a band-tile atlas in an SDRAM
memstore -- one bounded band at a time, so the decoded page is never resident
whole -- and served through a deliberately **small** `ra8_tile_cache`. The cache
holds fewer cells than a viewport spans, which forces LRU eviction and
decode-on-miss every frame; that pressure is the point of the app, not a
limitation of it. The viewport is the GLCDC panel itself, rendered by the shared
`mg_reader`: gray8 to RGB565, a status bar, and a minimap showing the viewport
rectangle over the page bounds.

## Navigation

Discrete tap zones: the four screen edges pan, a centre tap toggles between 1:1
and fit-page. There are no gestures because the GT911 path reports contacts, not
drags. Panning steps one tile per tap and clamps at the page bounds, so at
fit-page an edge tap is a no-op until a centre tap returns to 1:1.

The render is software gfx plus integer tile decode, so it is deterministic: the
boot banner is identical on the host twin
(`apps/board/stand_alone/ereader/tests/src/test_app_ereader_manga.c`), in
the emulator and on silicon, and the headless gate asserts a fixed render.

## Fixture provenance

`inc/mg_page_fixture.h` is `@generated` by `scripts/gen/gen_manga_page_fixture.py`:
a grayscale PNG (all rows filter 0) laid out as a grid of 256px tiles, each a
distinct solid gray with a black inner frame and a big blocky `C<col>R<row>`
label -- so panning visibly changes which labels are on screen. Solid tile blocks
compress small enough to bake into code MRAM. The host twin re-decodes a tile and
byte-checks its frame and fill grays against the generator, so **fixture and
reader cannot drift apart silently**.
