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
ELF=examples/ek_ra8d2/hw_pending/ereader_ui/build/ereader_ui.elf
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
- **D (done, #83):** real reflowed body text through `libs/ra_reflow`. When a
  font is present on the microSD (`FONT.OTF`, the SD-load path proven by
  `sd_font_render`), the Reading body is laid out live by `ra_reflow`
  (`ra_reflow_init` against the body rect -> `ra_reflow_layout_chapter` ->
  `ra_reflow_render_page_at(margin_x, body_top)`) at the proportional type
  scale, inset below the status bar and above the footer. The whole pipeline
  is heap-free (glyph arena in `ra_stbtt_alloc.c`; no-heap tokenizer
  `ra_reflow_tokenize.c`, #82). A `FONT.OTF` on the microSD overrides the baked
  face; verify that path with a card image, e.g.
  `tools/mkfontimg/build/mkfontimg libs/fonts/ArnoPro-Regular.otf /tmp/font.img FONT.OTF`
  then `board_sim <elf> --click 250 250 --sd /tmp/font.img --ppm out.ppm`
  (give it a generous `BOARD_SIM_MAX_CHUNKS` -- the ~400 KB SPI font read is
  slow under emulation; instant on real hardware).
- **E (done, #66):** a **Latin-1 subset of ArnoPro baked into internal flash**
  (`libs/fonts/arnopro_latin1.otf`, ~56 KB, generated into `.rodata` at build
  time by `scripts/utils/font_to_c.py`; see `libs/fonts/arnopro_latin1.h`). So the Reading body
  reflows **real proportional text with no SD card at all** -- with no `FONT.OTF`
  the body is now the baked reflow, not the old bitmap fallback (which remains
  only for a reflow-engine failure). The no-card golden (#84) was regenerated to
  the reflowed render. Reading from flash also sidesteps the slow emulated-SPI
  read, so `board_sim <elf> --click 250 250 --ppm out.ppm` (no `--sd`) shows live
  text.
- **F (done, #78/#80):** page-turn taps -- a tap in the right half of the body
  advances a `ra_reflow` page, the left half goes back (`ra_reflow_render_page_at`
  with a live page index; footer progress tracks it).
- **Next:** a Latin-1 face larger than the subset / full Unicode coverage would
  need external OSPI (blocked by #44); EPUB-driven chapter content (#69).

See issue #80 for the full chrome plan.
