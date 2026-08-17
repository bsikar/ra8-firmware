# ereader_svg

Rasterizes a baked SVG into a fixed RGB565 framebuffer and hashes the result
(#143) -- the vector counterpart to the raster stb_image gates `ereader_image`
(PNG) and `ereader_jpeg` (JPEG). It reads the document's intrinsic size from its
`viewBox`, maps that box onto the framebuffer, and fills the supported shapes
(`<rect>`, `<circle>`, `<polygon>`) through the `ra8_reflow` SVG renderer (#112,
#141). The rasterizer allocates nothing; its only side effect is drawing through
`ra8_gfx`. Headless -- no panel, SDRAM, touch or SD.

The SVG path is a separate rasterizer from stb_image, so the raster gates say
nothing about it. Here any drift in the SVG parse, the shape rasterizer or the
`viewBox`-to-box transform changes the hash. The renderer is host-tested in
`tests/test_ra8_svg.c`; this app is what runs it on the target and pins the
rendered output.
