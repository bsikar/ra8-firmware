# lcd_color_cycle

The first working GLCDC bring-up on the EK-RA8D2's 7-inch 1024x600 parallel TFT
(Renesas "Parallel Graphics Expansion Board 1"), and the canonical reference for
it. It cycles the GLCDC background-plane colour -- red, green, blue, white --
with no framebuffer fetched at all: the BG plane drives the panel on its own.
The blue LED toggles in step and the red LED latches on if any init step fails,
but the real check is a human looking at the panel.

## Four non-obvious requirements this bring-up surfaced

Every later LCD app inherits these through the HAL.

1. Routing a pin to the GLCDC peripheral does not set PDR. The LCD pins need a
   manual PFS write with `PSEL=0x19`, `PMR=1`, `PDR=1`.
2. The output stage composes `BG x GR2 x GR1`. Both graphics layers must be
   configured -- dimensions, alpha 0, transparent DISPSEL, `FLMRD=0` -- and
   VEN-asserted, even when only the BG plane is producing pixels.
3. The expansion board's backlight-enable signal is active HIGH.
4. `BG_BGC` is shadow-registered: a write takes effect only at the next VS, once
   `BG_EN.VEN` is asserted. The HAL's background-colour setter pulses VEN
   internally so callers do not have to.

The GLCDC pin set, plus the panel reset and backlight lines, is listed in
`main.c`. The authority for it is EK-RA8D2 v1 UM Table 33 "Parallel Graphics
Expansion Port Pin Assignments" p 42, and the controller is HUM Ch 63 "Graphics
LCD Controller (GLCDC)". The PLL1R/4 + DCDR /2 pixel clock that finally brought
the controller out of reset came from the LVGL EK-RA8D2 reference project.

## Cold boot is not 100% reliable

Roughly one power cycle in three the panel comes up in its no-signal white state
and stays there; power-cycling again brings it back. The firmware is
deterministic and the symptom is on the panel-controller side -- it looks like a
race between the LCD module's internal power-on reset and the GLCDC start
sequence. If the panel is white after a flash, unplug, wait a couple of seconds,
and plug back in.
