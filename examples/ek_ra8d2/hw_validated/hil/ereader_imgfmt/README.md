# ereader_imgfmt

Headless HIL gate for the **BMP + GIF** decode paths (#143) -- the last two
`stb_image` formats the firmware links that lacked an example.

## What it does

The firmware compiles four `stb_image` decoders (`STBI_ONLY_JPEG`, `_PNG`,
`_GIF`, `_BMP` in `stb_image_impl.c`). PNG (`ereader_image`, #106) and JPEG
(`ereader_jpeg`) already had a gate; this app exercises the remaining two:

1. `ra8_gfx_init` -- bind a 160x120 RGB565 framebuffer.
2. For **BMP** then **GIF**: clear the framebuffer, `ra8_img_decode_blit` the
   baked image (decode + aspect-preserving scale-to-fit + blit, allocating only
   from a fixed 128 KiB SRAM bump arena -- no `malloc`, NASA Rule 3), then
   FNV-1a-32 hash the framebuffer.

The console banner on success is:

```
ereader-imgfmt-hil: bmp=D53617C5 gif=350551C5 PASS
```

The fixtures (`imgfmt_fixtures.h`) are a 40x30 four-quadrant **BMP** and a 40x30
horizontal-band **GIF** -- two distinct patterns, so the two decode paths pin
two distinct CRCs. No panel / SDRAM / touch / SD dependency.

## Why this matters

Completes the stb_image format-coverage matrix: with PNG (#106), JPEG, and now
BMP + GIF all gated, any drift in any of the four linked decoders trips a gate.
Together with the EPUB cover-extraction (`ereader_cover`) and SVG vector
(`ereader_svg`) gates, this rounds out the cover-art image-decode family of
#143.

## Validation

Deterministic, so the ra8_emulator CRC gate is the regression net (the same way
`ereader_image` / `ereader_jpeg` gate). Run on `tools/ra8_emulator` (the
firmware boots, both decode + scale + hash passes run on the emulated M85, no
fault):

```
[uart] SCI8: ereader-imgfmt-hil: boot
[uart] SCI8: ereader-imgfmt-hil: bmp=D53617C5 gif=350551C5 PASS
```

## Regenerating the fixtures

```
cd examples/ek_ra8d2/hw_validated/hil/ereader_imgfmt
python3 make_imgfmt_fixtures.py   # rewrites imgfmt_fixtures.h (needs Pillow)
```

After changing an image, re-read the CRCs the board prints and update the
`HIL_EXPECT` line in `hil.conf`.

## Build

```
make ereader_imgfmt
make -C examples/ek_ra8d2/hw_validated/hil/ereader_imgfmt flash
```
