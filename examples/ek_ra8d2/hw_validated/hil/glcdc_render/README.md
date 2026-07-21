# glcdc_render

Headless **on-silicon HIL gate** for the **GLCDC layer-1 render path** (#121).

`ereader_chrome` gates the *software* rasteriser (`ra8_box` + `ra8_gfx` into
an SRAM buffer) but never touches the display controller. This app closes the
remaining gap: it proves the **GLCDC hardware** is programmed and scanning a
real framebuffer -- deterministically, headlessly, with no panel observer / no
SDRAM / no touch / no SD.

1. Paint a deterministic RGB565 pattern into a `512x512` SRAM framebuffer: a
   dark-blue field, a yellow corner-to-corner X, and a white 1-px border.
2. Bring the panel up through the display PAL
   (`display_init`, `k_display_backend_lcd_ra8_glcdc`), which runs the full GLCDC
   bring-up: panel power-on, GLCDC pin/clock setup, `ra8_glcdc_init`, background
   clear, `ra8_glcdc_start(true)`, `ra8_glcdc_layer1_show`.
3. Assert the hardware layer is actually programmed, three independent ways:
   - `display_init` returned `k_ra8_ok` with a live handle,
   - `display_get_framebuffer` reports *our exact* framebuffer pointer (GR1's
     AXI fetch is bound to this buffer), and
   - `ra8_glcdc_get_status` reads the GLCDC `SYS_STAT` register (the block is
     un-gated and reachable).
4. Fold an **FNV-1a-32** hash over the whole framebuffer and print it on the
   SCI8 J-Link OB console:

   ```
   glcdc-hil: layer1=ok dim=512x512 crc=B21B8D3D PASS
   ```

The gate (`hil.conf`, `uart_scrape`) asserts that line. The `layer1=ok ... PASS`
tail is emitted **only** once the three GLCDC-programmed asserts hold; any
bring-up failure prints `glcdc-hil: FAIL glcdc` and trips the negative match.
The render is deterministic (integer paint into a zeroed static buffer), so the
hash is the same every boot -- drift in the GLCDC path, the paint, or the
toolchain output changes it and trips the gate.

## Why the GLCDC status mask is not in the CRC

`SYS_STAT` (VSYNC/HSYNC/underflow) is a live, free-running register. It is
snapshotted into `g_glcdc_hil_status` for diagnostics but kept out of the
banner so the gate stays deterministic. The "GLCDC is programmed" proof is the
`display_init` success + framebuffer-binding triplet above, not the mask value.

## Build + run

```
make glcdc_render
scripts/hil/run_local.sh glcdc_render      # flash + scrape the banner
```

## board_sim validation (2026-06-20)

board_sim's panel compositor builds the panel image by reading the GLCDC GR1
framebuffer registers, so a `--ppm` snapshot independently confirms GR1 points
at the buffer:

```
tools/board_sim/build/board_sim \
  examples/ek_ra8d2/hw_validated/hil/glcdc_render/build/glcdc_render.elf \
  --ppm /tmp/glcdc_hil.ppm \
  --dump-sym g_glcdc_hil_ok --dump-sym g_glcdc_hil_crc
# [uart] SCI8: glcdc-hil: layer1=ok dim=512x512 crc=B21B8D3D PASS
# g_glcdc_hil_ok  = 1
# g_glcdc_hil_crc = 0xB21B8D3D   (deterministic across runs)
```

The PPM shows exactly the painted pattern (blue field, yellow X, white border)
in the 512x512 top-left layer over a black BG plane -- proof the GR1 registers
were programmed to scan `s_framebuffer`. CRC `B21B8D3D` is identical on every
run.

## Updating the baseline

After an **intentional** change to the render pattern or the GLCDC path,
recompute the hash (re-run board_sim, read `g_glcdc_hil_crc`) and update
`HIL_EXPECT` in `hil.conf` (and the value above). The on-device banner is the
source of truth.
