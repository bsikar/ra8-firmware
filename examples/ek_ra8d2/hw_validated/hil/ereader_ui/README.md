# ereader_ui

The e-reader device chrome on the live GLCDC panel (#80). The application shell
is laid out by the bounded box-model engine `ra8_box`, painted through `ra8_gfx`
into the GLCDC framebuffer, and navigated through the `ra8_ui` screen stack, in
the flat 16-level-grayscale visual language of the browser proof-of-concept.

- **Library** -- status bar, toolbar, a two-column grid of book cards (cover,
  title, author, reading-progress bar) and a bottom navigation strip: one
  `ra8_box` tree laid out once, then rendered.
- **Reading** -- status bar, reflowed body text at the reading margin, footer
  with page label and progress bar. A tap in the right half of the body advances
  a page and the left half goes back.

Navigation is live: the loop polls the GT911 touch controller, hit-tests taps
against the active screen's targets, and drives the screen stack.

Layout is resolution-adaptive. Every region derives from the framebuffer
dimensions the display backend reports, so a different panel descriptor reflows
the shell with no code change. The full RGB565 framebuffer lives in external
SDRAM.

The Reading body reflows real proportional text with no SD card at all: a
Latin-1 subset of Literata is baked into internal flash as `.rodata` at build
time. A `FONT.OTF` on the microSD overrides the baked face when one is present,
and the read is best-effort, so a board with no card simply falls back. The
whole pipeline is heap-free -- a glyph arena plus a no-heap tokenizer.

The chrome also reads a MAX17048-class fuel gauge (7-bit address `0x36`) over
the same I2C bus the touch controller uses, folds the state of charge into the
`ra8_batt` nag policy, and draws a persistent low-battery banner over whichever
screen is active, clearing once the battery recovers or is charging. A stock EVM
with no gauge fitted shows no banner.

Full validation is visual. The page-turn proof needs a human pressing SW1/SW2,
so the automated gate can only assert liveness: the main loop is a busy poll,
never WFI, so its free-running tick counter must keep advancing -- which proves
the app booted, brought SDRAM and the GLCDC up, and is not faulting, and nothing
more.

A Latin-1 face larger than the baked subset, or full Unicode coverage, would
need external OSPI (#44).
