# ereader_cover

The headline "show the book cover" pipeline, end to end (#143): pull the cover
image out of an `.epub`, decode it, scale it to fit, and render it. The EPUB is
opened in memory from a baked fixture (vendored miniz plus the bounded XML
reader, zero-heap through a static arena); `epub_get_cover_image` resolves
the `properties="cover-image"` manifest item; `ra8_img_decode_blit` decodes the
PNG with `stb_image`, nearest-neighbour scales it preserving aspect ratio, and
blits it into an RGB565 framebuffer -- allocating only from a fixed SRAM bump
arena, so the decode reaches no `malloc` (NASA Rule 3). The framebuffer is then
hashed.

The fixture cover is a portrait at a real 2:3 book ratio, and the target box is
landscape, so the fit is height-limited and the blitted image is narrower than
the box. Its approved PNG bytes are embedded in the generator and every EPUB
member is stored without compression, so regeneration does not depend on a host
image encoder or compressor. No panel, SDRAM, touch or SD dependency.

## It does not pass on silicon

On the bench `epub_open` fails outright on the real part, against the same
baked in-memory fixture the host tests use. The UART reader was attached before
the reset, so the #390 print-once race cannot explain it, and there is no SD
card, external hardware or provisioning anywhere in this path -- so it is a
firmware defect, not a rig gap (#170). `ra8_emulator` cannot arbitrate it
either: it stops on an Armv8.1-M encoding the Unicorn M33 model has no seam for.
`hil.conf` holds the capture. Re-promote only from a bench capture showing the
PASS banner and its CRC.

## Why it exists separately

`ereader_image` (#106) and `ereader_jpeg` (#143) prove the bare decode + scale +
blit pipeline for PNG and JPEG. This app adds the piece in front that the cover
use case actually needs -- pulling the image out of an EPUB manifest before
decoding it. Any drift in EPUB cover resolution, the PNG decoder, the scale math
or the toolchain trips the same deterministic hash gate.

Regenerate the fixture with
`examples/ek_ra8d2/hw_pending/ereader_cover/scripts/make_cover_fixture.py` for
`src/main.c`, then re-read the CRC the board prints and update `hil.conf`.
