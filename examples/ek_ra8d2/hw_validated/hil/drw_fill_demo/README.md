# drw_fill_demo

Clears a small ARGB8888 framebuffer in SRAM, has the DRW fill a rectangle into
it through `ra8_drw_fill_rect`, waits for the engine, then checks the result
byte-exact: the centre pixel must be filled and the corners must still be clear.
LED1 toggles on a clean fill and LED2 on a mismatch; `g_drw_match`, `g_drw_rev`
(HWREVISION), `g_drw_fill_err` and `g_drw_heartbeat` mirror the verdict for
headless SWD probing. Needs no external hardware.

The engine was long believed inert on silicon (#247). The real cause was power:
the D/AVE 2D block sits in the graphics power domain, which `PDCTRGD` gates OFF
at reset (HUM Ch 11.2.14 p 452), and cancelling the module-stop bit is not
enough. Once `ra8_drw_init` powers it, `HWREVISION` reads `0x0FBE0107` instead
of zero. A second defect was geometry: the engine paints the bounding box that
ORIGIN anchors, so the fill anchors ORIGIN at the rectangle's own top-left
rather than trying to place it with the spatial limiters, and sets
`CONTROL2.WRITEALPHA = 01` because its reset value takes alpha from COLOR2,
which is zero for a solid fill (#170). `tools/ra8_emulator` rasterizes the same
bounding box, so the verdict holds in the emulator and on hardware alike.

This is the register/immediate path. For a loop-stable clear+fill where the DRW
owns the framebuffer end to end and there is no CPU/engine write race, see
`drw_dlist_demo`.

## Notes (HUM R01UH1065EJ0130 Rev.1.30, Ch 62 "2D Drawing Engine")

- `ra8_drw_init` ungates the DRW, programs ORIGIN / PITCH / CONTROL2 to target
  the framebuffer (Ch 62.2 p 3685+), and masks the IRQs.
- `ra8_drw_fill_rect` programs the box and colour and kicks the enumeration
  unit; `ra8_drw_wait_idle` polls STATUS.busyenum (Ch 62.4 p 3725+).
- The framebuffer cache is left off (`enable_caches = false`) so the CPU reads
  the freshly-rasterized pixels. With it on, call `ra8_drw_cache_flush` before
  reading; and because the framebuffer lives in cacheable SRAM, invalidate the
  Cortex-M85 D-cache over its span before the verify, or place the framebuffer
  in a non-cacheable region.
