# lcd_demo

Two-layer GLCDC demo for the EK-RA8D2 7-inch 1024x600 parallel TFT
panel. Exercises the layer-2 (background) + layer-1 (UI overlay)
pipeline introduced in Sweep 2 of `libs/ra_hal/src/ra_glcdc.c` so the
GLCDC HAL has a real-board smoke test.

## Status: compiles clean. Hardware bring-up not yet validated.

The build target produces `lcd_demo.elf` / `.hex` / `.bin`. Flashing
and running on the EK-RA8D2 v1 board will require the user to
**confirm the GLCDC pin mapping against the EK-RA8D2 v1 board user
manual** (section "LCD Connector J57") -- the table in `main.c`
(`k_lcd_demo_glcdc_pins`) currently lists the chip-level
GLCDC-capable pads from the RA8D2 datasheet but is marked
`/* TODO confirm */` per-pin until that PDF lands in
`docs/reference/`.

## What it does

- `ra_cgc_init()` brings the chip up to its standard clock tree
  (CPUCLK0 = 1 GHz, PCLKA = 125 MHz). The GLCDC pixel clock
  (~51.2 MHz for 1024x600 @ 60 Hz) is sourced from PLL2 and
  programmed by the GLCDC driver itself when `ra_glcdc_init` runs
  (HUM Ch 53, "Graphics LCD Controller (GLCDC)", p 3744).
- 28 PFS routes are programmed to put the GLCDC parallel-RGB pin
  block (24 data + HSYNC + VSYNC + DE + CLK) in PSEL = `glcdc`
  mode (PSEL value 0x15).
- A 320 x 240 RGB565 framebuffer in SRAM is filled with a
  horizontal red-to-blue gradient and assigned to **layer 2**.
- A 128 x 128 RGB565 framebuffer in SRAM is assigned to **layer 1**;
  it gets a 64 x 64 magenta sprite redrawn every frame at a
  bouncing position.
- The compositor runs in `k_ra_blend_alpha` mode with global alpha
  `0xFF`, panel background `0x00000000` (black).
- LED1 toggles per frame as a visible heartbeat.

## Memory budget (why not full-resolution)

The on-chip SRAM is 1 MiB in the linker script. A native 1024 x 600
RGB565 framebuffer is **1.2 MiB** and does not fit. The demo uses
down-scaled layers placed at panel origin so the GLCDC two-layer
API path is exercised end-to-end without dragging in SDRAM bring-up.
Driving the panel at native resolution requires putting the
framebuffers in SDRAM (`.sdram_data` section of `linker_script.ld`)
plus initialising the SDRAM controller; that work is out of scope
for this smoke test.

## Pin-mux (TODO)

The EK-RA8D2 v1 board user manual is **not yet committed** to
`docs/reference/`. The pin table in `main.c` therefore documents
each entry with `/* TODO: confirm against EK-RA8D2 v1 board manual
table NN ("LCD Connector J57") */`. The PFS routing call succeeds
in the build because `ra_pfs_route_peripheral` only checks port/pin
range and the GLCDC PSEL value (0x15) is the same across all
GLCDC-capable pads.

Before flashing real hardware:

1. Pull the EK-RA8D2 v1 user manual from Renesas and confirm
   which port/pin pads on the 289-pin BGA are wired to J57's
   `LCD_DATA0..23`, `LCD_HSYNC`, `LCD_VSYNC`, `LCD_DE`, and
   `LCD_CLK`.
2. Update `k_lcd_demo_glcdc_pins[]` in `main.c` accordingly.
3. Re-run `make lcd_demo` to confirm the change still builds.

## Build

```sh
make lcd_demo            # from repo root
# or from this directory:
make
make flash               # via SEGGER J-Link OB
```

## References

- HUM Ch 53 "Graphics LCD Controller (GLCDC)", `docs/reference/r01uh1065ej0130-ra8d2.pdf`.
- EK-RA8D2 v1 UM Section 8.1 + Table 33 "Parallel Graphics Expansion
  Port Pin Assignments" p 42 -- canonical J1 pin map across RGB565 /
  RGB666 / RGB888 modes (matches `ra_board_glcdc_*_pins` in the BSP).
- `libs/ra_hal/src/ra_glcdc.c` -- two-layer driver implementation.
- `libs/ra_hal/inc/ra8d2_glcdc_regs.h` -- panel timing constants
  (`k_ra_glcdc_ek_*`).
- `libs/ra_board_ek_ra8d2` -- BSP `ra_board_glcdc_init` veneer that
  programs the J1 pin block per Table 33; this app currently bypasses
  the BSP and walks its own placeholder pin list, which is why the
  TODO comments above remain.

Uses `ra_board_ek_ra8d2` BSP for LED1 init/toggle (P600 per UM Table
24 p 31). The J1 panel pin programming is hand-rolled in main.c rather
than via `ra_board_glcdc_init` until the placeholder pin list is
reconciled with UM Table 33.

Validated 2026-05-02 against EK-RA8D2 v1 User's Manual (R20UT5523EG0101
Rev 1.01) Section 8.1 + Table 33 p 42 + Table 24 p 31, and HUM
(R01UH1065EJ0130) Ch 53 "GLCDC".
