# ereader_jpeg

Headless HIL gate for the **JPEG** cover-image decode pipeline (#143) -- the
JPEG counterpart to `ereader_image` (which covers PNG).

## What it does

Decodes a baked 120x90 RGB **JPEG** cover (`jpeg_fixture.h`) through the
zero-heap `ra8_img_decode_blit()` pipeline (the `stb_image` JPEG decoder,
allocating only from a fixed 128 KiB SRAM bump arena -- so the decode
reaches no `malloc`), nearest-neighbour scales it to fill a 160x120 RGB565
framebuffer in internal SRAM, FNV-1a-32 hashes the framebuffer, and prints:

```
ereader-jpeg-hil: img 160x120 crc=F71D21E8
```

No panel / SDRAM / touch / SD dependency.

## Why this matters

`stb_image` is compiled with **JPEG, PNG, GIF, and BMP** decoders
(`STBI_ONLY_*` in `stb_image_impl.c`), but only the **PNG** path had an
example (`ereader_image`, #106). This app exercises the **JPEG** path
end-to-end -- the format most book cover art actually ships in -- and
CRC-gates it so any drift in the JPEG decoder or the toolchain trips the
gate. Part of the cover-art image-decode family (#143).

## Validation

The render is deterministic (integer nearest-neighbour scale + fixed
RGB565 pack over a zeroed static framebuffer), so the hash is identical
every boot and identical on host / ra8_emulator / silicon. Captured baseline
`crc=F71D21E8` (stable across 3 ra8_emulator runs); `hil.conf` pins it.

```
make ereader_jpeg
# ra8_emulator: ereader-jpeg-hil: img 160x120 crc=F71D21E8
```

The fixture is the same 4-quadrant source image as the PNG `cover_fixture.h`,
JPEG-encoded (Pillow, quality 90); see `jpeg_fixture.h`.
