# WebP decode fixtures (#290)

Committed WebP bitstreams that exercise the vendored libwebp decoder
(`apps/shared_libs/third_party/libwebp/`) through the `ra8_webp` facade. They are decoded
by `tests/graphics/src/test_ra8_webp.c` and are also the seed corpus for the
`fuzz_ra8_webp` libFuzzer harness.

The test embeds each file's bytes inline (they are tiny) so it stays free of
runtime file I/O; these committed files are the reproducible provenance for
those inline arrays. Regenerate with the exact commands below and diff.

| File | Geometry | Codec | Purpose |
|------|----------|-------|---------|
| `fixture_lossless.webp` | 8x8 | VP8L (lossless) | Golden pixel round-trip: RGB is preserved bit-exact, so the decode is compared against the deterministic source pattern. |
| `fixture_lossy.webp` | 8x8 | VP8 (lossy) | Proves the lossy VP8 + YUV upsampling path decodes without crashing (pixels are not bit-compared -- lossy). |
| `fixture_wide.webp` | 8200x2 | VP8L (solid) | Header exceeds the 8192-per-axis width cap (dimension-guard vector). |
| `fixture_tall.webp` | 2x8200 | VP8L (solid) | Header exceeds the 8192-per-axis height cap (dimension-guard vector). |

## Regeneration

The lossless 8x8 pattern pixel at `(x, y)` is
`(r, g, b, a) = ((x*32) & 255, (y*32) & 255, ((x+y)*16) & 255, 255)`.

```sh
python3 - <<'PY'
from PIL import Image
img = Image.new("RGBA", (8, 8))
px = img.load()
for y in range(8):
    for x in range(8):
        px[x, y] = ((x*32) % 256, (y*32) % 256, ((x+y)*16) % 256, 255)
img.save("pattern.png")
Image.new("RGBA", (8200, 2), (10, 20, 30, 255)).save("wide.png")
Image.new("RGBA", (2, 8200), (10, 20, 30, 255)).save("tall.png")
PY

cwebp -lossless -exact -z 9 -o fixture_lossless.webp pattern.png
cwebp -q 80          -o fixture_lossy.webp    pattern.png
cwebp -lossless -exact -z 9 -o fixture_wide.webp     wide.png
cwebp -lossless -exact -z 9 -o fixture_tall.webp     tall.png
```

`-exact` keeps the RGB channels bit-exact under transparency so the lossless
golden is stable across hosts. Generated with libwebp `cwebp` v1.5.0.
