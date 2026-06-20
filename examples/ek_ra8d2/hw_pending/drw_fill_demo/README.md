# drw_fill_demo

DRW ("D/AVE 2D") hardware 2D-engine bring-up + fill-rect demo for the bare
EK-RA8D2 EVM. Exercises the `ra_drw` driver.

## What it does

Brings up SCI8 + LEDs + the DRW engine pointed at a 32x32 ARGB8888
framebuffer in SRAM. Once a second it:

1. Clears the framebuffer.
2. `ra_drw_fill_rect` -- fills a 16x16 green (`0xFF00FF00`) rectangle at
   (8, 8).
3. `ra_drw_wait_idle` -- waits (bounded) for the engine to finish.
4. Checks the framebuffer byte-exact: the centre pixel (16, 16) must be
   green and the corners must still be clear.
5. Reports `drw: fill match=Y` on the J-Link OB CDC channel.

- LED1 toggles on a clean fill; LED2 toggles on a mismatch.
- `g_drw_match` / `g_drw_rev` (HWREVISION) / `g_drw_fill_err` /
  `g_drw_heartbeat` mirror the result for headless J-Link probing.

No external hardware required.

## Why this is in hw_pending

`tools/board_sim` shadows the DRW control registers (the bring-up reads
back and `ra_drw_wait_idle` returns) but does **not** model the
rasterizer, so the fill never touches the framebuffer -- the centre pixel
stays clear and the banner reports `match=N` (`g_drw_match = 0`). The
actual rectangle fill can only be confirmed on silicon, so this app is
staged in `hw_pending/`.

## Notes (HUM R01UH1065EJ0130 Rev.1.30, Ch 62 "2D Drawing Engine")

- `ra_drw_init` ungates the DRW, programs ORIGIN / PITCH / CONTROL2 to
  target the framebuffer (HUM Ch 62.2 p 3685+), and masks the IRQs.
- `ra_drw_fill_rect` programs the box limiters + colour and kicks the
  enumeration unit; `ra_drw_wait_idle` polls STATUS.busyenum (HUM
  Ch 62.4 p 3725+).
- **Cache coherency:** this demo leaves the DRW FB cache off
  (`enable_caches = false`) so the CPU reads the freshly-rasterized
  pixels. On the bench, if you enable the FB cache, call
  `ra_drw_cache_flush` before reading; and because the framebuffer lives
  in cacheable SRAM, invalidate the Cortex-M85 D-cache over the
  framebuffer span before the verify (or place the FB in a non-cacheable
  region).

## On-silicon bench plan

1. `make drw_fill_demo`, then flash the EK-RA8D2.
2. Open the J-Link OB CDC channel at 115200 8N1; expect `drw: fill
   match=Y` once a second with LED1 toggling.
3. Or probe headless over SWD: `g_drw_match == 1`, `g_drw_rev` non-zero
   (the DRW HWREVISION), `g_drw_fill_err == 0`.
4. Optional: point the DRW at a real LCD/GLCDC layer framebuffer and
   render the rectangle on the panel to eyeball it.
5. Once green, move the app to `hw_validated/hil/`.

Build / flash:

```
make drw_fill_demo
make -C examples/ek_ra8d2/hw_pending/drw_fill_demo flash
```
