# lcd_draw_x

Draws a yellow X corner-to-corner on a dark-blue 512 x 512 RGB565
framebuffer in SRAM, composited via GLCDC graphics layer 1 over the
BG plane on the EK-RA8D2 7-inch 1024 x 600 parallel TFT (Renesas
"Parallel Graphics Expansion Board 1").  The blue board LED toggles
every 500 ms as a heartbeat; the red LED latches on if any init
step fails.

This is the canonical reference for "GLCDC layer-1 + pixel-level
framebuffer drawing".  Compared to `lcd_color_cycle` (which only
exercises the BG plane), this example demonstrates:

- Allocating a static, AXI-aligned SRAM framebuffer
- Pixel-level CPU writes into the framebuffer (`lcd_fb_fill`,
  `lcd_draw_x`)
- Flipping GLCDC graphics layer 1 from hidden to visible via
  `ra8_glcdc_layer1_show`
- The layer-1 / BG-plane composition (layer occupies the top-left
  512 x 512 of the panel; the remaining area is filled by the BG
  plane at `BG_BGC = 0x000000` = black)

## What to expect

After flashing and a power-cycle:

1. ~500 ms PLL / panel-controller settle.
2. The top-left 512 x 512 region of the panel becomes dark blue
   with a thick yellow X drawn corner-to-corner.
3. The rest of the panel (right ~512 x 600 strip, bottom ~1024 x 88
   strip) is solid black (BG plane).
4. The blue LED toggles every 500 ms.

If any init step fails, the red LED stays solid on and the panel
stays dark.

## Why 512 x 512 and not 1024 x 600

The on-board 64 MiB SDRAM at `0x68000000` is **not yet brought up**.
`ra8_sdramc_init` returns `k_ra8_ok` but writes to the SDRAM region are
silently dropped -- the SDRAMC pin routing + BSC clock + init
sequence are still TODO.  A full-panel 1024 x 600 RGB565 framebuffer
is 1.2 MiB and does not fit in the 1 MiB on-chip SRAM, so this demo
uses a 512 x 512 RGB565 framebuffer (0.5 MiB, fits in SRAM) and lets
the BG plane paint the rest of the panel.

Once SDRAM bring-up lands, future demos can use a full-resolution
framebuffer at `0x68000000` and drive the whole panel with GR1.

## Build / flash

```sh
make lcd_draw_x            # from repo root
# or from this directory:
make
make flash                 # via SEGGER J-Link OB
```

## References

- HUM Ch 63 "Graphics LCD Controller (GLCDC)",
  `docs/reference/r01uh1065ej0130-ra8d2.pdf`.
- EK-RA8D2 v1 UM Table 33 ("Parallel Graphics Expansion Port Pin
  Assignments"), p 42.
- `libs/ra8_hal/inc/ra8_glcdc.h` -- `ra8_glcdc_layer1_show` contract.
- `libs/ra8_board_ek_ra8d2` -- `ra8_board_glcdc_init` +
  `ra8_board_lcd_panel_power_on`.
- `examples/ek_ra8d2/hw_validated/manual/lcd_color_cycle` -- the
  BG-plane-only sibling demo this one extends.

## Validation

Validated visually on EK-RA8D2 v1 with Parallel Graphics Expansion
Board 1 on 2026-05-10.  Yellow X on blue square shows in the
top-left 512 x 512 panel area; the rest of the panel is solid black.
HIL flash via Pi + J-Link OB.
