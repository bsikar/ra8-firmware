# sd_font_render

Loads a TTF/OTF font off an **SD card** and renders a paragraph of XHTML with
`ra8_reflow` into the GLCDC framebuffer. This is the firmware promotion of the
host test `tests/test_ra8_sdmmc_card_reflow.c`: the exact same
`SD card -> ra8_fs -> ra8_reflow -> framebuffer` pipeline, but running as a real
RA8D2 binary (and inside `board_sim` against a `--sd` image).

The SD bring-up + font load is handled by the shared **`libs/ra8_sdfont`** helper,
which **self-provisions**: if the card carries no `FONT.OTF`, it writes a baked
Latin-1 font (`libs/fonts/literata_latin1.ttf`) to the card and reads it back. So
**any FAT-formatted "random" card just works** -- no host-side image prep. The
same helper backs `ereader_ui`'s (read-only) font load.

## Why

The e-reader needs its fonts in storage, not baked into flash. Issue #44 showed
the on-board Octo-SPI flash is not usable on this board, so font storage moved
to an SD card. This app is the end-to-end proof that the SD path works from a
real firmware image.

## Pipeline

```
SD card (FAT16, any -- font self-provisioned if absent)
  -> ra8_sdfont    (Pmod2/J25 SCI0 Simple-SPI -> ra8_sdmmc_spi -> ra8_fs; provisions FONT.OTF)
  -> ra8_reflow    (XHTML layout + stb_truetype rasterise)
  -> ra8_gfx       (RGB565 framebuffer in SDRAM)
  -> GLCDC        (scans out the panel)
```

## Run in the simulator (no hardware)

```sh
# Build board_sim + the FAT-image tool:
cmake -S tools/mkfontimg -B tools/mkfontimg/build && cmake --build tools/mkfontimg/build
make -C examples/ek_ra8d2/hw_validated/hil/sd_font_render

# A) Card already carrying a font:
tools/mkfontimg/build/mkfontimg libs/fonts/literata_latin1.ttf /tmp/font.img FONT.OTF
tools/ra8_emulator/build/ra8_emulator \
  examples/ek_ra8d2/hw_validated/hil/sd_font_render/build/sd_font_render.elf \
  --sd /tmp/font.img --ppm /tmp/out.ppm

# B) Blank "random" card -- exercises ra8_sdfont self-provisioning (text still renders):
tools/mkfontimg/build/mkfontimg --blank /tmp/blank.img
tools/ra8_emulator/build/ra8_emulator \
  examples/ek_ra8d2/hw_validated/hil/sd_font_render/build/sd_font_render.elf \
  --sd /tmp/blank.img --ppm /tmp/out.ppm
```

The rendered page appears in `/tmp/out.ppm` (and live with `--view`).

> **Heads-up on sim speed.** board_sim is a CPU emulator (Unicorn), so reading a
> font byte-by-byte over the emulated SPI bus and rasterising glyphs in software
> is far slower than on the 1 GHz panel (where it is instant). The compact
> `literata_latin1.ttf` (~37 KB) keeps the sim practical; a ~312 KB face may expire
> the default run budget. Raise it with the env knobs board_sim honours:
>
> ```sh
> BOARD_SIM_WALL_S=180 BOARD_SIM_MAX_CHUNKS=400000000 \
>   tools/ra8_emulator/build/ra8_emulator .../sd_font_render.elf --sd /tmp/font.img --ppm /tmp/out.ppm
> ```

## Run on hardware

Plug a Digilent PMOD MicroSD (410-380) into Pmod2 (J25) with **any** FAT-formatted
microSD card, then `make flash` and watch the panel. The card need not carry
`FONT.OTF` -- ra8_sdfont writes it on first boot if absent.

## HIL gate

`hil.conf` runs the `jlink_memprobe` mode: it dwells ~12 s (SDRAM settle + GLCDC
+ SD font read + reflow), then asserts `g_sfr_heartbeat` advances steadily. The
idle loop bumps that counter **only** after a clean render -- every failure stage
parks in `sfr_panic_halt`, freezing it -- so a steady advance proves the whole
pipeline ran. Run locally with `scripts/hil/run_local.sh sd_font_render`.

## Diagnostics

All SWD-readable (`mem32` over J-Link):

- `g_sfr_stage` -- bring-up progress (`6` = render_ok; `0x80..0x83` = failure stage).
- `g_sfr_err` -- raw `ra8_err_t` from `ra8_sdfont_load` on failure.
- `g_sfr_source` -- `0` = font read from the card, `1` = self-provisioned this boot.
- `g_sfr_font_len` / `g_sfr_pages` / `g_sfr_ink` -- loaded font size, page count, inked pixels.
- `g_sfr_heartbeat` -- idle-loop liveness (the HIL gate symbol).

On failure the panel is also flooded a stage-coded gray so a `--ppm` snapshot (or
a glance at the panel) shows where it stopped.
