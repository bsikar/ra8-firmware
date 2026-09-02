# ereader_imgfmt

Decodes a baked BMP and a baked GIF through `ra8_img_decode_blit()` into a fixed
RGB565 framebuffer and hashes each result (#143). The firmware links four
stb_image decoders; PNG (`ereader_image`) and JPEG (`ereader_jpeg`) already had
gates, and this app covers the remaining two, so drift in any of the four linked
decoders now trips something. Allocation comes only from a fixed SRAM bump
arena -- no `malloc`, NASA P10 Rule 3. Headless -- no panel, SDRAM, touch or SD.

The two fixtures are deliberately different patterns, a four-quadrant BMP and a
horizontal-band GIF, so the two decode paths pin two distinct hashes and neither
can pass on the other's output.
`examples/ek_ra8d2/hw_validated/hil/ereader_imgfmt/scripts/make_imgfmt_fixtures.py`
regenerates them (it needs Pillow); after changing an image the pinned hashes
have to be re-read off the board.
