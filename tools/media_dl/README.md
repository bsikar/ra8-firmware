<!--
Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
-->

# media_dl -- host CLI for the e-reader media downloader (v0)

A native **development-machine** executable (not an ARM ELF) that reuses the RA8
firmware's platform-agnostic code behind dependency-inversion seams, so the
downloader can be built and tested on a Mac/Linux box today -- long before the
ESP32-C6 Wi-Fi radio exists. The layers above the injected backend are the same
code the RA8 will run; only the leaf backends (network, storage) get swapped for
on-device drivers later.

"media" keeps the name broad: v0 targets serialized comics -- manga, manhwa,
manhua, webtoons -- which the firmware reads via `ra8_comic` / CBZ, with room to
grow into other downloadable media later.

## What v0 does (deliberately minimal)

Fetch one page URL -> scan it for `<img>` URLs -> download them politely into a
folder. That's it. No CBZ packaging, no image conversion, no per-site
descriptors, no discovery/search -- those are later milestones. The point of v0
is to prove the end-to-end host path and lock in the seam shapes.

## Design (why it maps cleanly to the RA8 later)

- `mdl_net.h` -- streaming HTTP GET **seam**, mirroring the on-device
  `ra8_ota_net_iface_t`. Host backend `mdl_net_curl.c` is libcurl (TLS,
  redirects, gzip, one reused connection with a persistent cookie jar). On the
  RA8 this becomes NetX Duo + Mbed TLS over the C6; callers do not change.
- `mdl_extract.{h,c}` -- v0 `<img>` scanner + relative-URL resolver. Replaced
  on-device by litehtml (already vendored) behind the same signature; per-site
  CSS selectors move into a config descriptor.
- `mdl_politeness.{h,c}` -- seeded, jittered inter-request delay. The full
  governor (global per-host token bucket, adaptive 429/503 backoff, Retry-After)
  is a later milestone.
- Return type is `ra8_err_t` from `libs/ra8_core` -- signatures are already
  device-shaped.

Intentional fixes vs. the Kotlin original: one User-Agent per session (not
per-request), one reused HTTP connection (shared pool + cookies), and a
`--attr data-src|src` default that prefers lazy-loaded image URLs.

## Build

```sh
cd tools/media_dl
cmake -B build -S .
cmake --build build -j
```

Requires libcurl (present on macOS by default) and a C23 host compiler
(Apple clang 21 works). Nothing here is cross-compiled to ARM.

## Run

```sh
./build/media_dl <page-url> [--out DIR] [--max N] \
                 [--attr data-src|src] [--seed S] [--timeout MS]
```

- `--out DIR`    output folder (default `out/`); files are `page_001.<ext>` ...
- `--max N`      stop after N images (default 0 = all); handy for a polite smoke test
- `--attr`       image attribute to prefer (default `data-src`, falls back to the other)
- `--seed S`     jitter seed (default 1; deterministic runs for testing)
- `--timeout MS` per-request budget (default 20000)

Example (single-page smoke test, two images):

```sh
./build/media_dl "https://en.wikipedia.org/wiki/Manga" --out /tmp/cdl --max 2
```

## Status / roadmap

v0 = fetch + extract + download loose images. Next: CBZ packaging (miniz +
`ra8_comic`), then RTA1 convert-on-download (`ra8_tileatlas_produce`) for the
webtoon/manhwa vertical-scroll reader, then config-driven per-site descriptors.
