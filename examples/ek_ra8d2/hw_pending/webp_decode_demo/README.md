# webp_decode_demo

On-target smoke test for the vendored **libwebp** decoder
(`libs/third_party/libwebp`) and the `ra8_webp` facade (#290). It proves the
WebP decode path cross-compiles, links, and runs on the Cortex-M85 with **zero
heap** -- every libwebp allocation is served from the caller-bound
`ra8_webp` bump arena (`_sbrk` traps after init).

## What it does

1. Brings up CGC / module-stop / SysTick and opens the J-Link OB VCOM console
   (SCI8, 115200 8N1).
2. Binds a 256 KiB static SRAM arena and decodes a baked 8x8 lossless (VP8L)
   WebP -- the same bytes as `tests/fixtures/webp/fixture_lossless.webp` -- into
   a static RGBA8888 framebuffer.
3. Verifies pixel `(1,1)` decoded to `(32, 32, 32, 255)` (the source pattern is
   lossless with `-exact`, so RGB is bit-exact).
4. Prints one banner line and parks:
   - `webp: PASS 8x8` on success,
   - `webp: FAIL` on any mismatch.

## Build / run

```sh
make          # cross-compile -> build/webp_decode_demo.elf / .hex / .bin
make flash    # J-Link load
make size     # arm-none-eabi-size on the ELF
```

Watch the banner on the VCOM console (115200 8N1).

## Status

`hw_pending` -- written and building for the target, not yet bench-validated on
silicon. It exercises only decode (no display); the render-path integration into
`ra8_reflow` / the band-tile format is #289.
