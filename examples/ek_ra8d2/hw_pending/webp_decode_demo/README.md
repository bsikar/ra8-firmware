# webp_decode_demo

On-target smoke test for the vendored libwebp decoder and the `ra8_webp` facade
(#290). It proves the WebP decode path cross-compiles, links and runs on the
Cortex-M85 with **zero heap**: every libwebp allocation is served from the
caller-bound `ra8_webp` bump arena, and `_sbrk` traps after init, so a decoder
that reached for the heap would fault rather than quietly work.

It binds a static SRAM arena, decodes a baked lossless (VP8L) WebP -- the same
bytes as `tests/fixtures/webp/fixture_lossless.webp` -- into a static RGBA8888
framebuffer, and checks one pixel against its expected value. The source is
lossless with `-exact`, so RGB is bit-exact and the check is not a tolerance.

It exercises decode only, with no display. Integrating the render path into
`reflow` and the band-tile format is #289.
