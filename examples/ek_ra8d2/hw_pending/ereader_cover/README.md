# ereader_cover

Headless HIL gate for the **EPUB cover-art** path (#143) -- the headline "show
the book cover" pipeline, end to end: pull the cover image out of an `.epub`,
decode it, scale it to fit, and render it.

## Status: demoted to hw_pending (#170 audit)

This app **does not pass on silicon** and lives under `hw_pending/`, not
`hw_validated/hil/`. On the EK-RA8D2 bench (UART reader attached before the
reset, so the #390 print-once race cannot explain it) it prints
`ereader-cover-hil: boot` then `ereader-cover-hil: FAIL open`: `ra8_epub_open`
fails outright on the real part against the same baked in-memory fixture the
host tests use. There is no SD card, external hardware, or provisioning in this
path, so this is a firmware defect, not a rig gap, and it is tracked. ra8_emulator
cannot arbitrate it either (it stops on an Armv8.1-M encoding the Unicorn M33
model has no seam for). See `hil.conf` for the full capture. Re-promote only
from a bench capture showing the PASS banner and its CRC.

## What it does

1. `ra8_epub_open` -- opens a baked, cover-bearing EPUB3 (`epub_cover_fixture.h`)
   **in memory** (vendored miniz ZIP + tinyxml2, zero-heap via the
   `ra8_epub_miniz_alloc` static arena).
2. `ra8_epub_get_cover_image` -- resolves the `properties="cover-image"` manifest
   item and copies the cover's raw PNG bytes into an SRAM buffer.
3. `ra8_img_decode_blit` -- decodes the PNG (`stb_image`), nearest-neighbour
   scales it to fit a 160x120 RGB565 framebuffer preserving aspect ratio, and
   blits it -- allocating only from a fixed 128 KiB SRAM bump arena, so the
   decode reaches no `malloc` (NASA Rule 3).
4. FNV-1a-32 hashes the rendered framebuffer and prints:

```
ereader-cover-hil: cover 80x120 crc=6E4E45C5 PASS
```

The fixture cover is a 96x144 portrait (a real 2:3 book-cover ratio); fitting it
into the 160x120 landscape box is height-limited, so the blitted size is 80x120.
No panel / SDRAM / touch / SD dependency.

## Why this matters

`ereader_image` (#106) and `ereader_jpeg` (#143) prove the bare
**decode + scale + blit** pipeline for PNG and JPEG. This app adds the piece in
front that the "book cover art" use case actually needs: pulling the cover
**out of an EPUB manifest** (`ra8_epub_get_cover_image`) before decoding it. It
is the EPUB analogue of `ereader_image` -- same deterministic CRC gate, with
the `ra8_epub` cover-resolution step added -- so any drift in EPUB cover
resolution, the stb_image PNG decoder, the scale math, or the toolchain trips
the gate. Part of the cover-art image-decode family (#143).

## Validation

Deterministic, so the ra8_emulator CRC gate is the regression net (the same way
`ereader_image` / `ereader_jpeg` / `epub_parse` gate). Run on
`tools/ra8_emulator` (the firmware boots, the full open -> extract -> decode ->
scale -> blit -> hash chain runs on the emulated M85, no fault):

```
[uart] SCI8: ereader-cover-hil: boot
[uart] SCI8: ereader-cover-hil: cover 80x120 crc=6E4E45C5 PASS
```

The `ra8_epub` parse + `stb_image` decode paths are already silicon-proven
(`epub_parse` #139, `ereader_image` #106); this app composes them, so
the ra8_emulator CRC gate plus those on-silicon precedents cover it end to end.

## Regenerating the fixture

```
cd examples/ek_ra8d2/hw_pending/ereader_cover
python3 make_cover_fixture.py     # rewrites epub_cover_fixture.h (needs Pillow)
```

After changing the cover, re-read the CRC the board prints and update the
`HIL_EXPECT` line in `hil.conf`.

## Build

```
make ereader_cover
make -C examples/ek_ra8d2/hw_pending/ereader_cover flash
```
