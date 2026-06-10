# sd_font_render

Loads a TTF/OTF font off an **SD card** and renders a paragraph of XHTML with
`ra_reflow` into the GLCDC framebuffer. This is the firmware promotion of the
host test `tests/test_ra_sdmmc_card_reflow.c`: the exact same
`SD card -> ra_sdmmc_spi -> ra_fs -> ra_reflow -> framebuffer` pipeline, but
running as a real RA8D2 binary (and inside `board_sim` against a `--sd` image).

## Why

The e-reader needs its fonts in storage, not baked into flash. Issue #44 showed
the on-board Octo-SPI flash is not usable on this board, so font storage moved
to an SD card. This app is the end-to-end proof that the SD path works from a
real firmware image.

## Pipeline

```
SD card (FAT16)                     <- font image, built by tools/mkfontimg
  -> ra_sdmmc_spi  (SPI ch1 + CS GPIO, Pmod2/J25)   <- genuine SD SPI-mode driver
  -> ra_fs         (FAT mount, open FONT.OTF)
  -> ra_reflow     (XHTML layout + stb_truetype rasterise)
  -> ra_gfx        (RGB565 framebuffer in SDRAM)
  -> GLCDC         (scans out the panel)
```

## Run in the simulator (no hardware)

```sh
# 1. Build the FAT-image tool and lay down a card image carrying the font
#    (written through real ra_fs so the firmware reads it back bit-for-bit):
cmake -S tools/mkfontimg -B tools/mkfontimg/build && cmake --build tools/mkfontimg/build
tools/mkfontimg/build/mkfontimg libs/fonts/ArnoPro-Regular.otf /tmp/font.img FONT.OTF

# 2. Cross-build the app:
make -C examples/ek_ra8d2/hw_pending/sd_font_render

# 3. Boot the real .elf on board_sim with the card attached, snapshot a PPM:
tools/board_sim/build/board_sim \
  examples/ek_ra8d2/hw_pending/sd_font_render/build/sd_font_render.elf \
  --sd /tmp/font.img --ppm /tmp/out.ppm
```

The rendered page appears in `/tmp/out.ppm` (and live with `--view`).

> **Heads-up on sim speed.** board_sim is a CPU emulator (Unicorn), so reading a
> 400 KB font byte-by-byte over the emulated SPI bus and rasterising glyphs in
> software is far slower than on the 1 GHz panel (where it is instant). The
> default run budget may expire first. Raise it with the env knobs board_sim
> honours, and/or use a smaller Latin font for a quick smoke:
>
> ```sh
> BOARD_SIM_WALL_S=1800 BOARD_SIM_MAX_CHUNKS=4000000 \
>   tools/board_sim/build/board_sim .../sd_font_render.elf --sd /tmp/font.img --ppm /tmp/out.ppm
> # or a ~100 KB font for a fast cycle:
> tools/mkfontimg/build/mkfontimg "/System/Library/Fonts/Supplemental/Andale Mono.ttf" /tmp/small.img FONT.OTF
> ```

## Run on hardware

Plug a Digilent PMOD MicroSD (410-380) into Pmod2 (J25) with a FAT-formatted
microSD card containing `FONT.OTF`, then `make flash` and watch the panel.

## Diagnostics

`g_sfr_stage` (J-Link / SWD readable) tracks bring-up progress; on failure the
panel is flooded a stage-coded gray so a `--ppm` snapshot shows where it stopped.
`g_sfr_font_len`, `g_sfr_pages`, and `g_sfr_ink` expose the loaded font size,
page count, and inked-pixel count.
