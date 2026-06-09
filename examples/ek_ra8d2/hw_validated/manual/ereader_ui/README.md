# ereader_ui

E-reader device **chrome** for the EK-RA8D2 (issue #80), rendered without
GUIX (which is being retired, #81). This app paints the application shell
around book content straight into the GLCDC framebuffer through
`libs/ra_gfx`, in the flat 16-level-grayscale / fixed-type-scale visual
language of the verified browser proof-of-concept ("PAPYR").

## What it renders (Phase A)

The **Reading** screen:

- **Status bar** -- wordmark, book title, clock + battery, hairline rule.
- **Body** -- chapter heading + paragraph text at the reading margin
  (placeholder prose: the public-domain opening of *The Time Machine*).
- **Footer** -- book title, page label, and a flat reading-progress bar.

Layout is resolution-adaptive: every region is derived from the
framebuffer dimensions the display backend reports (`ra_panel.h`), so a
different panel descriptor reflows the shell with no code change. The
full 1024x600 RGB565 framebuffer lives in external SDRAM
(`.sdram_data`).

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
- **A1:** real paginated body text through `libs/ra_reflow` (stb_truetype
  glyphs at the 48/34/24/18 type scale; embedded font).
- **B:** minimal `ra_reflow` box-model extension + the **Library** screen
  (grid of book cards) + page-turn / Library<->Reading controller.

See issue #80 for the full chrome-on-ra_reflow plan.
