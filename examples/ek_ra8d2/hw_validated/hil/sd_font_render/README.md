# sd_font_render

Loads a TTF/OTF font off an SD card and renders a paragraph of XHTML with
`ra8_reflow` into the GLCDC framebuffer -- the firmware promotion of the host
test `tests/test_ra8_sdmmc_card_reflow.c`, running the same
SD -> `ra8_fs` -> `ra8_reflow` -> `ra8_gfx` -> GLCDC pipeline as a real RA8D2
binary.

The e-reader needs its fonts in storage rather than baked into flash, and #44
showed the on-board Octo-SPI part was not usable for that on this board, so
font storage moved to the card. This app is the end-to-end proof the path
works.

## Any card works

SD bring-up and the font load go through the shared `libs/ra8_sdfont` helper,
which **self-provisions**: if the card carries no font file, it writes a baked
Latin-1 face and reads it back. So any FAT-formatted card just works, with no
host-side image prep. `ereader_ui` uses the same helper read-only. On hardware
that means a Digilent PMOD MicroSD in Pmod2 (J25) with any FAT card in it.

## Reading a failure

The gate watches `g_sfr_heartbeat`, which the idle loop bumps **only** after a
clean render -- every failure stage parks instead, freezing it -- so a steady
advance proves the whole pipeline ran rather than that the firmware survived.
When it does not advance, these are all SWD-readable:

- `g_sfr_stage` -- bring-up progress, or a failure stage in the `0x80` range.
- `g_sfr_err` -- the raw `ra8_err_t` from the font load.
- `g_sfr_source` -- font read from the card, or self-provisioned this boot.
- `g_sfr_font_len` / `g_sfr_pages` / `g_sfr_ink` -- font size, page count,
  inked pixels.

On failure the panel is also flooded a stage-coded gray, so a snapshot or a
glance at the screen shows where it stopped.

Under the emulator this pipeline is genuinely slow: reading a font byte by byte
over an emulated SPI bus and rasterising glyphs in software is nothing like the
1 GHz part, where it is instant. That is why the baked face is a compact
Latin-1 subset -- a full-size face can outrun the default run budget.
