<!--
Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
-->

# media_dl -- host CLI for the e-reader media downloader (v1)

A native **development-machine** executable (not an ARM ELF) that reuses the RA8
firmware's platform-agnostic code behind dependency-inversion seams, so the
downloader can be built and tested on a Mac/Linux box today -- long before the
ESP32-C6 Wi-Fi radio exists. The layers above the injected backend are the same
code the RA8 will run; only the leaf backends (network, storage) get swapped for
on-device drivers later.

"media" keeps the name broad: it targets serialized comics -- manga, manhwa,
manhua, webtoons -- which the firmware reads via `ra8_comic` / CBZ, with room to
grow into other downloadable media later.

## What it does

**series mode** (the real one): read a per-site descriptor, list a series'
chapters, and download the first N as folders of page images:

```
downloads/<series-slug>/<chapter>/page_001.jpg ...
```

**page mode** (debug): fetch one URL and download its `<img>` URLs.

No CBZ packaging or image conversion yet -- those are the next milestones. The
scope is deliberately small (unlike the half-baked Kotlin original): fetch,
extract, download, politely.

## Design (why it maps cleanly to the RA8 later)

- `mdl_net.{h,c}` -- streaming HTTP GET **seam**, mirroring the on-device
  `ra8_ota_net_iface_t`. Host backend is libcurl (TLS, redirects, gzip, one
  reused connection with a persistent cookie jar). On the RA8 this becomes NetX
  Duo + Mbed TLS over the C6; callers do not change.
- `mdl_extract.{h,c}` -- `<img>`/`<a>` tag scanner + relative-URL resolver.
  Replaced on-device by litehtml (already vendored) behind the same signatures.
- `mdl_config.{h,c}` -- flat key=value **site descriptor** loader. Adding a site
  is dropping a `.conf` in `sites/`, no rebuild. Fixed-size struct, zero dynamic
  allocation -- ports to the RA8 unchanged.
- `mdl_politeness.{h,c}` -- seeded, jittered inter-request delay. The full
  governor (global per-host token bucket, adaptive 429/503 backoff, Retry-After)
  is a later milestone.
- Return type is `ra8_err_t` from `libs/ra8_core` -- signatures are already
  device-shaped.

Intentional fixes vs. the Kotlin original: one User-Agent per session (not
per-request), one reused HTTP connection (shared pool + cookies), and image
extraction that prefers `data-src` (lazy-loaded) with a URL-substring filter to
drop loader/nav/ad images.

## Build

```sh
cd tools/media_dl
cmake -B build -S .
cmake --build build -j
```

Requires libcurl (present on macOS by default) and a C23 host compiler
(Apple clang 21 works). Nothing here is cross-compiled to ARM.

## Run

Series mode:

```sh
./build/media_dl --config sites/manhwaus.conf \
                 --series "https://manhwaus.net/webtoon/nano-machine/" \
                 --chapters 2 [--start K] [--out DIR] [--seed S] [--timeout MS]
```

- `--config FILE`  site descriptor (see `sites/manhwaus.conf`)
- `--series URL`   the series page URL
- `--chapters N`   how many chapters to download (default 1)
- `--start K`      skip the first K chapters (default 0)
- `--out DIR`      output root (default `downloads/`)

Page mode (debug):

```sh
./build/media_dl <page-url> [--out DIR] [--max N] [--attr data-src|src]
```

## Site descriptors

A `.conf` is flat `key = value` (`#` comments, `[section]` lines ignored). See
`sites/manhwaus.conf`. Keys: `name`, `host`, `kind`, `chapter_url_contains`,
`chapter_order` (`asc`|`reverse`|`doc`), `page_img_attr`,
`page_img_url_contains`, and the `*_delay_min/max` politeness bounds. Sites
behind a Cloudflare JS challenge will not work (no challenge solver yet).

## Export formats (`--format`)

| `--format` | How it's made | Needs |
|------------|---------------|-------|
| `loose` (default) | page images in a folder | nothing |
| `cbz` | vendored miniz ZIP writer (STORE) | nothing |
| `cbt` | hand-written POSIX ustar tar | nothing |
| `cbt.gz` | miniz DEFLATE + RFC-1952 gzip framing | nothing |
| `cbt.xz` | tar, then the external `xz` CLI (CRC32 check, 1 MiB dict) | `xz` on PATH |
| `cbr` | the external `rar` CLI | `rar` on PATH |
| `epub` | a valid EPUB3 of the pages via vendored miniz (`ra8_epub` opens it) | nothing |
| `rta1` | per-page native RTA1 tile atlas via the firmware `ra8_tileatlas` producer -- a full-width column (`tile_w == width`) the `ra8_webtoon` engine opens directly | nothing |
| `rabook` | build a CBZ, then `tools/epub_compile/cbz_compile.py` -> the RBKC `.rabook` | `python3` + Pillow |

`cbz`/`cbt`/`cbt.gz`/`epub`/`rta1` are fully self-contained (in-tree/vendored
code, no system library or external process): `epub` is hand-built with miniz,
and `rta1` reuses the firmware's own `ra8_tileatlas_produce` host-side, so a
`.rta1` the CLI writes is byte-identical to one the RA8 produces (webtoon
column). `cbt.xz` / `cbr` / `rabook` are optional -- they shell out to `xz` /
`rar` / `python3` only when producing that format and report clearly if the tool
is absent (RAR and an xz *encoder* have no small in-tree option; the RBKC
container has no C writer, so rabook uses the desktop python emitter). The
`.gz`/`.xz` variants wrap a whole tar and are opened on-device by
`ra8_comic_open_wrapped`. RTA1 writes one `page_NNN.rta1` per page into the
chapter folder (the webtoon-native form), not a single archive file.

## Status / roadmap

Working: config-driven series -> chapter list -> per-chapter page images,
downloaded politely (verified against manhwaus.net). Known limits: naive
extension naming (Content-Type/magic-byte typing is a planned fix), single-site
descriptor format (richer TOML later), no proxy/Cloudflare handling. Next: CBZ
packaging (miniz + `ra8_comic`), then RTA1 convert-on-download
(`ra8_tileatlas_produce`) for the webtoon/manhwa vertical-scroll reader.
