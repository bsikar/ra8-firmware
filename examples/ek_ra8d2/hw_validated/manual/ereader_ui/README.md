# ereader_ui

E-reader device **chrome** for the EK-RA8D2 (issue #80). The application
shell is laid out by the bounded box-model engine `libs/ra_box`, painted
into the GLCDC framebuffer through `libs/ra_gfx`, and navigated through
the `libs/ra_ui` screen stack -- in the flat 16-level-grayscale visual
language of the verified browser proof-of-concept ("PAPYR"). Book content
reflow (`libs/ra_reflow`) is a later milestone; the Reading body here
uses the bundled bitmap font.

## What it renders

- **Library** -- status bar, toolbar (search + count), a 2-column grid of
  book cards (cover + title + author + reading-progress bar), and a bottom
  navigation strip. The whole screen is a `ra_box` tree (stacks + grid +
  padding/gap + fixed/flex sizing) laid out once, then rendered.
- **Reading** -- status bar, body text at the reading margin (the
  public-domain opening of *The Time Machine*), footer with page label and
  a flat reading-progress bar.

**Navigation is live:** the loop polls the GT911 touch controller
(`ra_touch`); a tap is hit-tested against the screen's targets
(`ra_ui_hit_test`) and drives the `ra_ui` screen stack -- tapping a book
card opens the Reading view, tapping the Reading status bar goes back.

Layout is resolution-adaptive: every region derives from the framebuffer
dimensions the display backend reports (`ra_panel.h`), so a different
panel descriptor reflows the shell with no code change. The full 1024x600
RGB565 framebuffer lives in external SDRAM (`.sdram_data`).

## Run it in the simulator

```
make sim-ereader_ui            # boot the real .elf on tools/board_sim
```

Headless render + navigation proof:

```
make ereader_ui                                  # cross-build the .elf
ELF=examples/ek_ra8d2/hw_validated/manual/ereader_ui/build/ereader_ui.elf
tools/board_sim/build/board_sim "$ELF" --ppm /tmp/library.ppm       # Library
tools/board_sim/build/board_sim "$ELF" --click 250 250 --ppm /tmp/reading.ppm  # tap a card -> Reading
```

`--click` injects a tap through the genuine GT911 -> I2C -> `ra_touch`
path, so the navigation is exercised exactly as on hardware.

## Roadmap (this example)

- **A (done):** Reading chrome via `ra_gfx`, verified in `board_sim`.
- **B (done):** `ra_box` box-model layout + the **Library** screen (book
  grid) + `ra_ui` screen-stack navigation.
- **C (done):** live touch input -- GT911 tap -> `ra_ui_hit_test` ->
  screen change, proven with `board_sim --click`.
- **Next:** real paginated body text through `libs/ra_reflow` (stb_truetype
  glyphs at the 48/34/24/18 type scale). Allocator blocker for the glyph
  rasteriser is **solved**: `stb_truetype` allocates per-glyph scratch
  (vertices + rasteriser edge/point lists) on *both* the one-shot and the
  Box+Make paths, so `ra_reflow_render` now (a) keeps the glyph bitmap in a
  fixed buffer via `stbtt_GetCodepointBitmapBox` + `stbtt_MakeCodepointBitmap`
  and (b) redirects `STBTT_malloc`/`STBTT_free` to the no-heap bump arena in
  `libs/ra_reflow/src/ra_stbtt_alloc.c` (bump + refcount auto-reset; sized
  3x the measured 32 KiB worst case). Verified heap-free against the bundled
  ArnoPro face for the full printable-ASCII set at 96 px.
  The old tinyxml2 parse blocker is **also solved**: `ra_reflow` now parses
  with the no-heap streaming tokenizer (`ra_reflow_tokenize.c`, #82), so there
  is no `MemPoolT`/`new` growth on the chapter path. **Remaining for on-target
  reading text:** (1) provision a TTF for the body -- only the bitmap
  `ra_gfx_font` is embedded today, ra_reflow needs an stb_truetype face baked
  in as a `static const` array or loaded off the microSD; (2) render into the
  body sub-region -- `ra_reflow_render_page()` targets a viewport-width
  framebuffer at origin (0,0), so it needs a destination stride + (x0,y0) to
  sit below the status bar and above the footer; (3) wire the Reading body to
  `ra_reflow_layout_chapter` + `ra_reflow_render_page` in place of the bitmap
  font. Tracked as #83 (see also #80).

See issue #80 for the full chrome plan.
