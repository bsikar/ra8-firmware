# drw_blend_demo

Composites three layers into a small ARGB8888 framebuffer in SRAM -- an opaque
background fill, an opaque sprite fill over it, then a source-over alpha blend
at global alpha -- and folds an FNV-1a-32 hash over the result. It covers the
`ra8_drw` blend path that `drw_fill_demo` (solid fill only) does not (#120).
Needs no panel and no external hardware.

The pinned hash is a real EK-RA8D2 capture, checked pixel by pixel against
source-over arithmetic at that alpha. It matters that it is: an earlier golden
was FNV-1a-32 over an all-zero framebuffer -- the demo hashing its own untouched
BSS, because the D/AVE 2D engine had never rasterized a pixel on this silicon --
and it carried "silicon-confirmed PASS" status for months. Two stacked defects
had to be fixed before a real render existed: the graphics power domain is gated
off at reset (#247), and the driver then placed rectangles with the spatial
limiters while leaving `CONTROL2.WRITEALPHA` at its reset value (#170). If the
hash ever changes, re-derive it from a bench capture -- never from the emulator,
and never by writing down whatever the demo happens to print.

LED1 toggles on a clean render and LED2 on a DRW error. `g_drw_blend_crc`,
`g_drw_blend_err`, `g_drw_blend_heartbeat` and `g_drw_blend_fb` mirror the
result for headless SWD probing.

## Notes (HUM R01UH1065EJ0130 Rev.1.30, Ch 62 "2D Drawing Engine")

- `ra8_drw_init` ungates the engine and programs ORIGIN / PITCH / CONTROL2 for
  ARGB8888 over the framebuffer (Ch 62.2 p 3685+).
- `ra8_drw_set_blend` writes CONTROL2.{USEACB,BSF,BDF,BSFA,BDFA,BDIA} and the
  COLOR1 alpha byte (Ch 62.2.2 p 3691-3692, Ch 62.4.7 p 3729). Every DRW
  register except STATUS and PERFCOUNT is write-only, so the driver shadows
  CONTROL2 / COLOR1 in software rather than reading them back.
- The DRW framebuffer cache is left off (`enable_caches = false`) so the CPU
  reads the freshly-composited pixels. With it on, call `ra8_drw_cache_flush`
  before reading; and because the framebuffer lives in cacheable SRAM,
  invalidate the Cortex-M85 D-cache over its span before hashing, or place the
  framebuffer in a non-cacheable region.
