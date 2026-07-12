# lcd_color_cycle

First proven-working GLCDC bring-up on the EK-RA8D2 7-inch 1024x600
parallel TFT (Renesas "Parallel Graphics Expansion Board 1").  Cycles
the GLCDC background-plane color (red -> green -> blue -> white) every
500 ms.  No framebuffer fetched -- the BG plane drives the panel on
its own.

The blue board LED toggles each cycle as a heartbeat; the red LED
latches on if any init step fails.

## What to expect

After flashing:

1. Board boots, panel reset is pulsed low for 50 ms then released
   high, backlight (`P514` BLEN) is asserted high.
2. GLCDC is configured for 1024x600 @ 60 Hz parallel RGB888 output
   sourced from PLL1R / 4, DCDR = /2 (~50 MHz pixel clock).
3. The whole panel turns **solid red**, then 500 ms later
   **green**, then **blue**, then **white**, and repeats.
4. The blue LED toggles in step with every color change.

If any step fails, the red LED stays solid on and the panel stays
dark.

## Why this app exists

This is the canonical reference for "GLCDC + parallel-RGB panel up
and running".  The HAL bring-up surfaced four non-obvious
register-level requirements that any future LCD app will inherit
through the HAL; they are documented in the header of `main.c`:

1. `ra8_pfs_route_peripheral` does not set PDR; LCD pins need a
   manual PFS write with PSEL=0x19, PMR=1, PDR=1.
2. The GLCDC output stage composes `BG x GR2 x GR1`.  BOTH GR1 and
   GR2 must be configured (dimensions, alpha=0, DISPSEL=transparent,
   FLMRD=0) and VEN-asserted, even when only the BG plane is
   producing pixels.  The fix lives in
   `libs/ra8_hal/src/ra8_glcdc.c::internal_panel_program`.
3. The Parallel Graphics Expansion Board's BLEN signal (`P514`) is
   active-HIGH.
4. `BG_BGC` is shadow-registered.  Writes only take effect at the
   next VS once `BG_EN.VEN=1` is asserted;
   `ra8_glcdc_set_background_color` pulses VEN internally so callers
   don't need to.

## Pin map

28 GLCDC pins per EK-RA8D2 v1 User Manual Table 33 ("Parallel
Graphics Expansion Port Assignments").  Listed in `main.c`
`k_lcd_glcdc_pins[]`.

| Group        | Pins                                    |
|--------------|-----------------------------------------|
| DATA0..7  (B) | P914 P915 P903 P902 P910 P911 P912 P913 |
| DATA8..15 (G) | P904 P207 P11_7 P11_6 P11_5 P11_1 P11_4 P11_3 |
| DATA16..23(R) | P11_2 P11_0 P707 P711 P712 P713 P714 P715 |
| TCON / CLK    | P806 (VSYNC) P805 (HSYNC) P807 (DE) P515 (CLK) |

Plus two GPIO control signals:

- `P606` -- panel RESET_L (active-low)
- `P514` -- BLEN (backlight enable, active-high)

## Build / flash

```sh
make lcd_color_cycle          # from repo root
# or from this directory:
make
make flash                    # via SEGGER J-Link OB
```

## References

- HUM Ch 63 "Graphics LCD Controller (GLCDC)",
  `docs/reference/r01uh1065ej0130-ra8d2.pdf`.
- EK-RA8D2 v1 UM Table 33 ("Parallel Graphics Expansion Port Pin
  Assignments"), p 42.
- LVGL EK-RA8D2 reference project -- source of the PLL1R / 4 + DCDR =
  /2 clock config that brought the controller out of reset.
- FSP `r_glcdc.c` -- reference open/start sequence (BG + GR1 + GR2 +
  OUT_VLATCH all VEN-asserted).
- `libs/ra8_hal/src/ra8_glcdc.c` -- HAL implementation; see comments
  in `internal_panel_program` for the GR2 lower-layer requirement.

## Known caveat: flaky cold boot

Cold-boot success is **not 100%** on this hardware combination.  On
roughly 1 in 3 power cycles the panel comes up in its no-signal
white state and stays there.  Re-power-cycling once or twice always
brings it back to cycling.  The firmware itself is deterministic;
the symptom is on the panel-controller side and seems related to a
race between the LCD module's internal POR and our GLCDC start
sequence.

Workaround: if the panel is white after `make flash`, unplug USB,
wait ~2 s, plug back in.  Repeat if needed.

Future fix ideas (not done yet):
- Read `BG_MON` after start and re-run the start sequence if the
  controller isn't reporting "running".
- Increase the post-reset settle delay in `lcd_panel_power_on`.
- Probe `LCD_CLK` with a scope on a flaky boot vs a good boot to
  confirm the failure is in the panel, not the chip.

## Validation

Validated visually on EK-RA8D2 v1 with Parallel Graphics Expansion
Board 1 on 2026-05-10.  Panel cycles colors exactly as described
above on successful cold boots; HIL flash via Pi + J-Link OB.
