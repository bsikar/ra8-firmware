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

Layout is resolution-adaptive: every region derives from the framebuffer
dimensions the display backend reports (`ra_panel.h`), so a different
panel descriptor reflows the shell with no code change. The full 1024x600
RGB565 framebuffer lives in external SDRAM (`.sdram_data`).

## Run it in the simulator

```
make sim-ereader_ui            # boot the real .elf on tools/board_sim
```

Headless render proof:

```
make ereader_ui                                  # cross-build the .elf
tools/board_sim/build/board_sim \
  examples/ek_ra8d2/hw_validated/manual/ereader_ui/build/ereader_ui.elf \
  --ppm /tmp/ereader.ppm
```

## Roadmap (this example)

- **A (done):** Reading chrome via `ra_gfx`, verified in `board_sim`.
- **B (done):** `ra_box` box-model layout + the **Library** screen (book
  grid) + `ra_ui` screen-stack navigation.
- **Next:** real paginated body text through `libs/ra_reflow` (stb_truetype
  glyphs at the 48/34/24/18 type scale; embedded font); live touch input
  routing (`ra_ui` hit-testing) for Library <-> Reading on hardware.

See issue #80 for the full chrome plan.
