# ereader_manga -- viewable pan/zoom manga reader (Demo B)

A **viewable** reader for a page far larger than the 1024x600 panel. The baked
page is transcoded into a JOF band-tile atlas (`libs/ra8_jof`) and paged
**decode-on-demand** through a small `ra8_tile_cache`, with the viewport being
the GLCDC panel itself. Navigation is discrete **tap-zones** (board_sim's GT911
model has no gestures): the four screen edges pan the viewport, a centre tap
toggles zoom.

## What it does

1. Boots clocks / MSTP / SysTick / LEDs, then SDRAM + the 1024x600 GLCDC panel
   (`ra8_display_pal` with `k_display_backend_lcd_ra8_glcdc`), and binds
   `ra8_gfx` to the RGB565 framebuffer in SDRAM.
2. Transcodes the baked **1536x2048** grayscale PNG page (`mg_page_fixture.h`,
   larger than the screen) through `ra8_jof_produce` into a 6x8 = **48**
   deflate-coded 256x256 tile JOF atlas in an SDRAM memstore -- one bounded
   band at a time, the decoded page never resident whole.
3. Serves the atlas through a deliberately small **4-cell** `ra8_tile_cache`
   (decode-on-miss = one bounded `ra8_jof_read_tile` inflate into the
   pinned cell), so a viewport spanning more tiles than the cache holds forces
   LRU eviction every frame.
4. Renders the current viewport crop to the panel with the shared `mg_reader`:
   gray8 -> RGB565, a top status bar (`MANGA  1:1  x=.. y=..`), and a
   bottom-right minimap showing the viewport rectangle over the page bounds.
5. Prints one deterministic banner over the SCI console at boot so the headless
   SIL gate still asserts a fixed render:

   `ereader-manga: page 1536x2048 tiles=48 atlas=20384 view=0,0 zoom=1:1 crc=5CBD900B ok`

The render (software gfx + integer tile decode) is deterministic, so the banner
is identical on the host twin (`tests/test_app_ereader_manga.c`), board_sim, and
silicon.

## Navigation (tap-zones)

| Tap zone (panel px)                 | Action                          |
|-------------------------------------|---------------------------------|
| Top edge band                       | Pan the viewport up             |
| Bottom edge band                    | Pan down                        |
| Left edge band                      | Pan left                        |
| Right edge band                     | Pan right                       |
| Centre                              | Toggle zoom 1:1 <-> fit-page    |

Panning is one 256px step per tap and clamps at the page bounds; at fit-page the
whole page is shown, so an edge tap is a no-op until a centre tap returns to 1:1.

## Fixture provenance

`mg_page_fixture.h` is **@generated** by
`scripts/gen/gen_manga_page_fixture.py`: a 1536x2048 8-bit grayscale PNG (all
rows filter 0, IDAT `zlib.compress(..., 9)`) laid out as a 6x8 grid of 256px
tiles, each a distinct solid gray with a black inner frame and a big blocky
`C<col>R<row>` label -- so panning visibly changes which labels are on screen.
Solid tile blocks compress to ~20 KB of PNG, small enough to bake into the 1 MB
code MRAM. The host twin re-decodes a tile and byte-checks its frame + fill
grays against the generator, so fixture and reader cannot drift apart silently.

Regenerate with `python3 scripts/gen/gen_manga_page_fixture.py`.

## Status

Sim-viewable (`hw_pending`): the panel bring-up + tile pipeline are fully
modelled by board_sim (`--ppm` / `--view` show the real reader screen); not yet
run on a bench board.

## Build / view / drive

```
make ereader_manga                      # cross-build from the repo root
make sim-ereader_manga                  # live macOS window; click to navigate

# Headless snapshot to a viewable image (initial 1:1 top-left):
tools/ra8_emulator/build/ra8_emulator \
    examples/ek_ra8d2/hw_pending/ereader_manga/build/ereader_manga.elf \
    --panel tools/ra8_emulator/panels/ek_ra8d2.toml --ppm topleft.ppm

# Drive navigation headless (one tap lands per run):
#   right edge  -> pan right   |  bottom edge -> pan down  |  centre -> zoom
tools/ra8_emulator/build/ra8_emulator <elf> --panel <toml> --touch-seq "950:300" --ppm panned.ppm
tools/ra8_emulator/build/ra8_emulator <elf> --panel <toml> --touch-seq "512:300" --ppm fit.ppm
```
