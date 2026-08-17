# ereader_chrome

Builds a representative e-reader chrome screen as an `ra8_box` tree -- a status
bar over a two-column grid of book cells -- lays it out into a fixed frame,
renders the boxes and labels into an RGB565 framebuffer in internal SRAM with
the bundled bitmap font, and hashes the framebuffer (#76, #80). It closes the
real-hardware gap left by the emulator golden renders, with no panel, SDRAM,
touch or SD dependency.

Integer layout, a fixed bitmap font and a zeroed static framebuffer make the
render deterministic, so the hash is the same every boot and the same one the
identical render produces on host -- byte-for-byte agreement between the host
render and RA8D2 silicon. Drift in the box-model math, the software rasteriser
or the toolchain output changes it.
