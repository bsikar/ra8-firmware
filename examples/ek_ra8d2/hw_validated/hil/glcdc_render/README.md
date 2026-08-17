# glcdc_render

Proves the GLCDC hardware is programmed and scanning a real framebuffer (#121).
`ereader_chrome` gates the software rasteriser into an SRAM buffer but never
touches the display controller; this app paints a deterministic RGB565 pattern
into an SRAM framebuffer, brings the panel up through the display PAL -- panel
power-on, GLCDC pin and clock setup, init, background clear, start, layer-1 show
-- and hashes the framebuffer. No panel observer, SDRAM, touch or SD needed.

"The GLCDC is programmed" is asserted three independent ways: the PAL init
returns a live handle; `display_get_framebuffer` reports our exact framebuffer
pointer, so GR1's AXI fetch is bound to this buffer; and `ra8_glcdc_get_status`
reads `SYS_STAT`, so the block is un-gated and reachable. Only once all three
hold does the app emit its pass tail; a bring-up failure prints a distinct
failure line instead.

**The `SYS_STAT` mask is deliberately kept out of the hash.** VSYNC, HSYNC and
underflow are live free-running bits, and folding them in would make the gate
non-deterministic. The mask is snapshotted into `g_glcdc_hil_status` for
diagnostics only; the programmed-ness proof is the triplet above, not the mask
value.

The emulator's panel compositor builds its image by reading the GLCDC GR1
framebuffer registers, so a rendered snapshot from an emulator run independently
confirms GR1 really points at the buffer, rather than replaying what the
firmware believes.
