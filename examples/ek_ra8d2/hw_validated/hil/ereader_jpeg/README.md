# ereader_jpeg

Decodes a baked RGB JPEG cover through the zero-heap `ra8_img_decode_blit()`
pipeline, nearest-neighbour scales it into a fixed RGB565 framebuffer in
internal SRAM, and hashes the framebuffer (#143) -- the JPEG counterpart to
`ereader_image`, which covers PNG. JPEG is the format most book cover art
actually ships in. Allocation comes only from a fixed SRAM bump arena, so the
decode reaches no `malloc`. Headless -- no panel, SDRAM, touch or SD.

Integer nearest-neighbour scaling and a fixed RGB565 pack over a zeroed static
framebuffer make the render deterministic, so the same hash appears on host,
under the emulator and on silicon, and any drift in the JPEG decoder or the
toolchain trips it. The fixture is the same source image as the PNG gate's,
JPEG-encoded, so the two gates differ only in the decoder under test.
