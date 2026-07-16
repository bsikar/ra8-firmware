# ereader_webtoon -- continuous vertical-scroll (webtoon / manhwa) mode (#289)

The third e-reader reading mode, beside reflowable EPUB text (`ra8_reflow`) and
paged CBZ/manga (`ra8_comic`): a chapter is one **continuous vertical strip** --
tall image slices stacked seamlessly and read by scrolling, with **no page
boundaries**. This app is the headless, deterministic gate for the scroll
engine (`libs/ra8_webtoon`).

## What it proves

1. **RTA1 as the band format.** A webtoon band is one full-width RTA1 tile
   column (`tile_w == width`, `libs/ra8_tileatlas`), so the tile index *is* the
   band index: O(1) seek to any scroll position. The app builds a tall synthetic
   strip (100 x 1000, twenty 100 x 50 bands) as a raw RTA1 atlas in SRAM; each
   pixel encodes its absolute canvas coordinate so a wrong band placement would
   corrupt the render (and the CRC).
2. **Continuous scroll, 0-skip.** `ra8_webtoon` scrolls the whole strip top ->
   bottom; every visible band is composited each frame. The banner's
   `skipped=0` is the seamless contract -- no band is ever dropped or skipped
   over during the traversal.
3. **Bounded memory.** Bands page decode-on-demand through a 4-cell
   `ra8_tile_cache` (fewer cells than the 20 bands), so the resident
   decoded-pixel set stays constant regardless of strip height or scroll
   distance; the LRU evicts and re-decodes as the viewport moves.
4. **DRW-ready composite.** The engine hands each band's on-screen sub-window to
   a blit seam; here it is the software `ra8_gfx_blit` (RGB888 -> RGB565). On
   silicon the same seam binds `ra8_drw_blit_textured_rect` for a zero-copy
   SDRAM -> framebuffer blit.

## Banner (the golden)

```
ereader-webtoon: bands=20 steps=29 skipped=0 crc=482C6145
```

The render is a deterministic integer pipeline (RTA1 raw read, RGB565 pack,
FNV-1a-32 over every frame), so the CRC is identical on the unit-test host, in
board_sim and on silicon (SIM == HIL). The host twin
`tests/test_ra8_webtoon.c` proves the same 0-skip and per-pixel seamless render
over the same strip geometry. `hil.conf` pins the banner; the board-sim smoke
gate (`scripts/board_sim_smoke.sh`) asserts it in CI.

## Build / run

```sh
make                                   # cross-compile -> build/ereader_webtoon.elf
../../../../tools/board_sim/build/board_sim build/ereader_webtoon.elf   # emulate
```

## Status

`hw_pending`: **sim-validated** (board_sim, deterministic banner) -- the on-chip
render is bench-runnable on a stock EK-RA8D2 (SCI8 / J-Link OB VCOM only, no
external hardware), but has not yet been run on the rig. Promote to
`hw_validated/hil/` after a bench run confirms the same CRC.
