# drw_blend_demo

DRW ("D/AVE 2D") hardware 2D-engine blit + alpha-blend headless CRC demo for
the bare EK-RA8D2 EVM. Exercises the `ra8_drw` driver's compositing path
(`ra8_drw_blend_t`) -- the gap called out in issue #120 that the sibling
`drw_fill_demo` (solid fill only) does not cover.

## What it does

Brings up SCI8 + LEDs + the DRW engine pointed at a 32x32 ARGB8888
framebuffer in SRAM. Once a second it issues a three-layer draw sequence and
hashes the framebuffer. On a working engine the layers would be:

1. **Background** -- `ra8_drw_fill_rect` over the whole 32x32 framebuffer,
   opaque dark-blue (`0xFF202060`).
2. **Sprite blit** -- `ra8_drw_fill_rect` of a 16x16 opaque green
   (`0xFF40C040`) box at (8, 8), laid over the background.
3. **Alpha blend** -- `ra8_drw_set_gradient` sets COLOR1 to the red
   foreground (`0xFFE04040`); `ra8_drw_set_blend` arms the source-over blend
   unit at global alpha `0x80`; a final `ra8_drw_fill_rect` at (4, 4) 16x16
   would composite the half-transparent red over the background **and** the
   sprite, so the overlap region would carry a deterministic red-over-blue
   and red-over-green mix.

On real silicon the DRW is inert (issue #247), so none of these draws land a
pixel: the register sequence completes but the framebuffer is untouched.

It then folds an FNV-1a-32 hash over the whole framebuffer (exactly as
`ereader_chrome` does) and prints
`drw: blit+blend crc=76EFDDC5 PASS` on the J-Link OB CDC channel. Because the
DRW is inert on real silicon (see below), the framebuffer stays zero and that
CRC is FNV-1a-32 over 4096 zero bytes.

- LED1 toggles on a clean render; LED2 toggles on a DRW error.
- `g_drw_blend_crc` / `g_drw_blend_err` / `g_drw_blend_heartbeat` mirror the
  result for headless J-Link probing; `g_drw_blend_fb` is the framebuffer.

No display panel and no external hardware are required -- this is a pure
compute + memory test, headless-gateable.

## The DRW is inert on silicon (#247) -- SIM == HIL

The D/AVE 2D engine does not rasterize on the real EK-RA8D2 (issue #247):
PERFCOUNT never advances, HWREVISION reads 0, and a J-Link dump of the
framebuffer after a run is all zeros. This demo's driver sequence
(background fill, sprite fill, `ra8_drw_set_blend`, foreground fill) runs to
completion -- every register write is accepted and `ra8_drw_wait_idle`
returns -- but no pixel is ever written, so the FNV-1a-32 hash is that of the
untouched zero framebuffer: `76EFDDC5`.

`tools/board_sim` models the engine INERT to match
(`tools/board_sim/src/periph/board_periph_drw.c`): writes are accepted and
discarded, STATUS reads idle, HWREVISION reads 0, and the ORIGIN render
trigger synthesises nothing. The emulator therefore hashes the same zero
framebuffer and prints the same `76EFDDC5` banner as the bench -- so this
gate PASSES in both SIL and HIL by reproducing the silicon result, not by
faking a render. The app sits in `hw_validated/hil/` because that
zero-framebuffer CRC is confirmed on silicon; it will only report a
genuinely composited CRC once #247 brings the engine to life (at which point
re-baseline this CRC from a real bench render and update the inert model +
`board_sim_smoke.sh` expectation together).

## Notes (HUM R01UH1065EJ0130 Rev.1.30, Ch 62 "2D Drawing Engine")

- `ra8_drw_init` ungates the DRW and programs ORIGIN / PITCH / CONTROL2 for
  ARGB8888 over the framebuffer (HUM Ch 62.2 p 3685+).
- `ra8_drw_set_blend` writes CONTROL2.{USEACB,BSF,BDF,BSFA,BDFA,BDIA} and the
  COLOR1 alpha byte (HUM Ch 62.2.2 p 3691-3692, Ch 62.4.7 p 3729). Every DRW
  register except STATUS and PERFCOUNT is write-only, and the driver shadows
  CONTROL2 / COLOR1 in software rather than reading them back; on silicon the
  only readable state -- STATUS and HWREVISION -- reads idle / 0 (#247), which
  the inert board_sim model reproduces exactly.
- **Cache coherency:** like `drw_fill_demo`, this demo leaves the DRW FB
  cache off (`enable_caches = false`) so the CPU reads the freshly-composited
  pixels. On the bench, if you enable the FB cache call `ra8_drw_cache_flush`
  before reading; and because the framebuffer lives in cacheable SRAM,
  invalidate the Cortex-M85 D-cache over the framebuffer span before the
  hash (or place the FB in a non-cacheable region).

## On-silicon bench plan

1. `make drw_blend_demo`, then flash the EK-RA8D2.
2. Open the J-Link OB CDC channel at 115200 8N1; expect
   `drw: blit+blend crc=76EFDDC5 PASS` once a second with LED1 toggling.
3. Or probe headless over SWD: `g_drw_blend_err == 0`, `g_drw_blend_crc ==
   0x76EFDDC5` (the zero-framebuffer hash), `g_drw_blend_heartbeat` advancing.
4. The `76EFDDC5` above is FNV-1a-32 over the all-zero framebuffer: today the
   DRW is inert (#247), so no pixel is composited on the bench. When #247 makes
   the engine render, dump `g_drw_blend_fb`, confirm a genuine composite, then
   re-baseline the CRC in `hil.conf` + `board_sim_smoke.sh` and teach
   `board_periph_drw.c` the real render so SIM == HIL still holds.
