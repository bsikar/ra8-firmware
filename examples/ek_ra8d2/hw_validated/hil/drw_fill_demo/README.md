# drw_fill_demo

DRW ("D/AVE 2D") hardware 2D-engine bring-up + fill-rect demo for the bare
EK-RA8D2 EVM. Exercises the `ra8_drw` driver.

## What it does

Brings up SCI8 + LEDs + the DRW engine pointed at a 32x32 ARGB8888
framebuffer in SRAM. Once a second it:

1. Clears the framebuffer.
2. `ra8_drw_fill_rect` -- fills a 16x16 green (`0xFF00FF00`) rectangle at
   (8, 8).
3. `ra8_drw_wait_idle` -- waits (bounded) for the engine to finish.
4. Checks the framebuffer byte-exact: the centre pixel (16, 16) must be
   green and the corners must still be clear.
5. Reports `drw: fill match=Y` on the J-Link OB CDC channel.

- LED1 toggles on a clean fill; LED2 toggles on a mismatch.
- `g_drw_match` / `g_drw_rev` (HWREVISION) / `g_drw_fill_err` /
  `g_drw_heartbeat` mirror the result for headless J-Link probing.

No external hardware required.

## Why this is hw_validated (issue #247 resolved)

The DRW was thought inert on silicon, but the real cause was that the D/AVE 2D
engine sits in the graphics power domain, which `PDCTRGD` gates OFF at reset
(HUM Ch 11.2.14 p 452). `ra8_drw_init` now powers it (`HWREVISION` reads
`0x0FBE0107`), and with the ORIGIN-anchored bounding-box fill (no spatial
limiters, `WRITEALPHA = 01`) the engine paints exactly pixels (8,8)..(23,23) in
`0xFF00FF00` -- byte-verified by a J-Link savebin. `tools/board_sim`
(`board_periph_drw.c`) rasterizes the same bounding box, so `match=Y` holds in
SIL and on hardware alike (SIM == HIL).

This demo exercises the register/immediate fill path (`ra8_drw_fill_rect`),
which renders a byte-clean single rectangle. For a loop-stable clear+fill where
the DRW owns the framebuffer end to end (no CPU write race), see
`drw_dlist_demo` -- the display-list route.

## Notes (HUM R01UH1065EJ0130 Rev.1.30, Ch 62 "2D Drawing Engine")

- `ra8_drw_init` ungates the DRW, programs ORIGIN / PITCH / CONTROL2 to
  target the framebuffer (HUM Ch 62.2 p 3685+), and masks the IRQs.
- `ra8_drw_fill_rect` programs the box limiters + colour and kicks the
  enumeration unit; `ra8_drw_wait_idle` polls STATUS.busyenum (HUM
  Ch 62.4 p 3725+).
- **Cache coherency:** this demo leaves the DRW FB cache off
  (`enable_caches = false`) so the CPU reads the freshly-rasterized
  pixels. On the bench, if you enable the FB cache, call
  `ra8_drw_cache_flush` before reading; and because the framebuffer lives
  in cacheable SRAM, invalidate the Cortex-M85 D-cache over the
  framebuffer span before the verify (or place the FB in a non-cacheable
  region).

## On-silicon bench plan

The DRW renders on silicon once the graphics power domain is up (#247 resolved):
the banner reads `match=Y` and the framebuffer reads back byte-clean.

1. `make drw_fill_demo`, then flash the EK-RA8D2.
2. Open the J-Link OB CDC channel at 115200 8N1; the banner reads
   `drw: fill match=Y` with LED1 toggling.
3. Or probe headless over SWD: `g_drw_match == 1`, `g_drw_rev == 0x0FBE0107`
   (DRW HWREVISION once powered), and `g_drw_fill_err == 0`. A `savebin` of
   `g_drw_fb` shows pixels (8,8)..(23,23) all `0xFF00FF00` on a zero background.
4. Optional: point the DRW at a real LCD/GLCDC layer framebuffer and
   render the rectangle on the panel to eyeball it.

Build / flash:

```
make drw_fill_demo
make -C examples/ek_ra8d2/hw_validated/hil/drw_fill_demo flash
```
