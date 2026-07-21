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

## Why this is in hw_pending

The DRW is inert on real silicon (issue #247): the D/AVE 2D engine never
rasterizes -- PERFCOUNT never advances, HWREVISION reads 0, and a J-Link
dump of the framebuffer after a run is all zeros. `tools/board_sim` models
the engine INERT to match (`tools/board_sim/src/board_periph_drw.c`): the
`ra8_drw_fill_rect` register sequence is accepted and `ra8_drw_wait_idle`
returns, but no pixel is written, so the centre pixel stays clear and the
banner honestly reports `match=N` (`g_drw_match = 0`) in the emulator, just
as it does on the bench. The `board_sim_smoke.sh` gate keys on that
`match=N` line. This app cannot report `match=Y` -- in SIL or on hardware --
until #247 brings the engine to life; hold it in `hw_pending/` until then.
When the engine renders on silicon, teach `board_periph_drw.c` the real
rasterizer (so SIM == HIL still holds), flip this expectation to `match=Y`,
and promote the app to `hw_validated/hil/`.

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

Today the DRW is inert (issue #247): steps 2-3 report the failure state
(`match=N`, `g_drw_rev == 0`) on both the bench and board_sim. They describe
the target state that becomes reachable once #247 makes the engine render.

1. `make drw_fill_demo`, then flash the EK-RA8D2.
2. Open the J-Link OB CDC channel at 115200 8N1; while #247 is open the banner
   reads `drw: fill match=N` with LED2 toggling. Once the engine renders it
   flips to `drw: fill match=Y` with LED1 toggling.
3. Or probe headless over SWD: while #247 is open `g_drw_match == 0`,
   `g_drw_rev == 0` (the DRW HWREVISION reads 0 on inert silicon), and
   `g_drw_fill_err == 0` (the register sequence still completes cleanly). A
   working engine yields `g_drw_match == 1` and a non-zero `g_drw_rev`.
4. Optional: point the DRW at a real LCD/GLCDC layer framebuffer and
   render the rectangle on the panel to eyeball it.
5. Once `match=Y` is confirmed on silicon (#247), move the app to
   `hw_validated/hil/`.

Build / flash:

```
make drw_fill_demo
make -C examples/ek_ra8d2/hw_validated/hil/drw_fill_demo flash
```
