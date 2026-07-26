# ereader_svg

Headless HIL gate for the **SVG vector-art** render path (#143) -- the vector
counterpart to `ereader_image` (PNG) and `ereader_jpeg` (JPEG), which
cover the raster `stb_image` path.

## What it does

1. `ra8_gfx_init` -- binds a 160x120 RGB565 framebuffer in internal SRAM.
2. `ra8_svg_size` -- reads the baked SVG's intrinsic size from its `viewBox`.
3. `ra8_svg_render` -- maps the `viewBox` onto the framebuffer box and rasterizes
   the supported shapes (`<rect>` / `<circle>` / `<polygon>` filled) through the
   `ra8_reflow` SVG renderer (#112/#141). No allocation (NASA Rule 3) -- the
   rasterizer's only side effect is drawing through `ra8_gfx`.
4. FNV-1a-32 hashes the rendered framebuffer and prints:

```
ereader-svg-hil: svg 100x100 crc=A6450BE6 PASS
```

where `100x100` is the SVG's intrinsic `viewBox` size. The baked fixture
(`svg_fixture.h`) is a small vector "cover" -- a background rect, a circle, a
bar, and a triangle, each a distinct solid colour. No panel / SDRAM / touch / SD
dependency.

## Why this matters

The cover-art family needs both raster and vector inputs. `stb_image` covers
PNG/JPEG/BMP/GIF (`ereader_image` #106, `ereader_jpeg`), but SVG is a
separate, distinct rasterizer (`ra8_reflow` SVG, #112/#141) with no example gate.
This app renders an SVG to a fixed framebuffer and CRC-gates it, so any drift in
the SVG parse, the shape rasterizer, or the `viewBox` -> box transform trips the
gate. Together with the PNG/JPEG raster gates and the EPUB cover-extraction gate
(`ereader_cover`), this completes the PNG/JPEG/SVG cover-art coverage of
#143.

## Validation

Deterministic, so the board_sim CRC gate is the regression net (the same way
`ereader_image` / `ereader_jpeg` gate). Run on `tools/ra8_emulator` (the
firmware boots, the SVG size + render + hash chain runs on the emulated M85, no
fault):

```
[uart] SCI8: ereader-svg-hil: boot
[uart] SCI8: ereader-svg-hil: svg 100x100 crc=A6450BE6 PASS
```

The `ra8_reflow` SVG rasterizer is already host-tested (`tests/test_ra8_svg.c`,
#112/#141); this app runs it on the target and pins the rendered output.

## Build

```
make ereader_svg
make -C examples/ek_ra8d2/hw_validated/hil/ereader_svg flash
```
