# drw_blend_demo

DRW ("D/AVE 2D") hardware 2D-engine blit + alpha-blend headless CRC demo for
the bare EK-RA8D2 EVM. Exercises the `ra_drw` driver's compositing path
(`ra_drw_blend_t`) -- the gap called out in issue #120 that the sibling
`drw_fill_demo` (solid fill only) does not cover.

## What it does

Brings up SCI8 + LEDs + the DRW engine pointed at a 32x32 ARGB8888
framebuffer in SRAM. Once a second it renders three layers and hashes the
result:

1. **Background** -- `ra_drw_fill_rect` over the whole 32x32 framebuffer,
   opaque dark-blue (`0xFF202060`).
2. **Sprite blit** -- `ra_drw_fill_rect` of a 16x16 opaque green
   (`0xFF40C040`) box at (8, 8), laid over the background.
3. **Alpha blend** -- `ra_drw_set_gradient` sets COLOR1 to the red
   foreground (`0xFFE04040`); `ra_drw_set_blend` arms the source-over blend
   unit at global alpha `0x80`; a final `ra_drw_fill_rect` at (4, 4) 16x16
   composites the half-transparent red over the background **and** the
   sprite, so the overlap region carries a deterministic red-over-blue and
   red-over-green mix.

It then folds an FNV-1a-32 hash over the whole framebuffer (exactly as
`ereader_chrome` does) and prints
`drw: blit+blend crc=F0AE5DC5 PASS` on the J-Link OB CDC channel.

- LED1 toggles on a clean render; LED2 toggles on a DRW error.
- `g_drw_blend_crc` / `g_drw_blend_err` / `g_drw_blend_heartbeat` mirror the
  result for headless J-Link probing; `g_drw_blend_fb` is the framebuffer.

No display panel and no external hardware are required -- this is a pure
compute + memory test, headless-gateable.

## Why this is in hw_pending

`tools/board_sim` models BOTH the DRW solid-fill rasterizer AND the
global-alpha source-over blend op
(`tools/board_sim/src/board_periph_drw.c`): a `ra_drw_fill_rect` issued
while `CONTROL2.USEACB` is set reads each destination pixel back and mixes
the COLOR1 source in with the integer source-over math
`out = (src*a + dst*(255-a) + 127) / 255` per channel (ARGB8888 only) --
the same combination the silicon performs. So the emulated framebuffer hash
equals the baked-in expected CRC and the `board_sim_smoke.sh` gate is green
on the emulator today. The app stays in `hw_pending/` until that CRC is
confirmed on silicon (the simulator proves the driver register sequence and
the blend arithmetic, not the real D/AVE 2D engine).

## Notes (HUM R01UH1065EJ0130 Rev.1.30, Ch 62 "2D Drawing Engine")

- `ra_drw_init` ungates the DRW and programs ORIGIN / PITCH / CONTROL2 for
  ARGB8888 over the framebuffer (HUM Ch 62.2 p 3685+).
- `ra_drw_set_blend` writes CONTROL2.{USEACB,BSF,BDF,BSFA,BDFA,BDIA} and the
  COLOR1 alpha byte (HUM Ch 62.2.2 p 3691-3692, Ch 62.4.7 p 3729). Because
  the driver read-modify-writes CONTROL2, this register must read back its
  last-written value; the board_sim model honours that and seeds the
  HWREVISION stamp as the CONTROL2 reset value so an early
  `ra_drw_get_hwrevision` still reads a non-zero "engine present" code.
- **Cache coherency:** like `drw_fill_demo`, this demo leaves the DRW FB
  cache off (`enable_caches = false`) so the CPU reads the freshly-composited
  pixels. On the bench, if you enable the FB cache call `ra_drw_cache_flush`
  before reading; and because the framebuffer lives in cacheable SRAM,
  invalidate the Cortex-M85 D-cache over the framebuffer span before the
  hash (or place the FB in a non-cacheable region).

## On-silicon bench plan

1. `make drw_blend_demo`, then flash the EK-RA8D2.
2. Open the J-Link OB CDC channel at 115200 8N1; expect
   `drw: blit+blend crc=<8hex> PASS` once a second with LED1 toggling.
3. Or probe headless over SWD: `g_drw_blend_err == 0`, `g_drw_blend_crc`
   matching the printed value, `g_drw_blend_heartbeat` advancing.
4. Confirm the printed CRC matches the emulator's `F0AE5DC5`. If it differs,
   the silicon blend rounding / channel order does not match the model --
   re-baseline the expected CRC in `hil.conf` and `board_sim_smoke.sh` and
   document the silicon math.
