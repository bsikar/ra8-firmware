# Image Pyramid

`image_pyramid` deliberately makes a JPEG look worse at every level. It removes
every other row and column without filtering, encodes the compacted pixels at
JPEG quality 25, then decodes that file before making the next level. This is a
software version of physically shredding a photograph and pushing the retained
strips together.

The host app uses the repository's `ra8_jpeg_sw_decode()` and
`ra8_jpeg_sw_encode()` implementations. It does not use PPM, a platform image
codec, or a third-party JPEG API.

```sh
just apps::host::build image_pyramid
mkdir -p output
just apps::host::run image_pyramid args="apps/host/image_pyramid/fixtures/dog-source.jpg --out-dir output"
just quality::devcontainer::test-zig
```

Run these commands from the repository root. The default command writes eight
baseline JPEG files. `--levels N` selects 1 through 16 levels when the source
dimensions permit them. Each result has a name such as
level-01-165x124-q25.jpg, and every generated JPEG is decoded before the next
level is produced so its quality loss compounds. The command refuses existing
final or temporary output names and removes files it published if processing
later fails. The output directory must not be modified concurrently.

This is an intentional degradation demonstration, not a production-quality
image resizer: it performs no interpolation, averaging, or filtering.

SPDX-License-Identifier: MIT
Copyright (c) 2026 Brighton Sikarskie
