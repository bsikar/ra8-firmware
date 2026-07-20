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
chapters, download the first N, and package them into a reader-openable file.
By default the N chapters **combine into one archive** named for the chapter
range:

```
downloads/<series-slug>/<slug>-<lo>-<hi>.<ext>     e.g. nano-machine-1-2.cbz
```

Pass `--separate` for one archive per chapter instead, or `--format loose` (the
default when `--format` is omitted) to stop at page-image folders and skip
packaging. Continuous page numbering across chapters means the combined archive
reads as one contiguous book.

**pack mode** (`--pack DIR --format FMT`): package an existing folder of page
images -- no network. Handy for re-encoding a download into another format and
for the integration harness.

**page mode** (debug): fetch one URL and download its `<img>` URLs.

The scope is deliberately small (unlike the half-baked Kotlin original): fetch,
extract, download politely, package.

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

Series mode (combined by default -- two chapters into one `nano-machine-1-2.cbz`):

```sh
./build/media_dl --config sites/manhwaus.conf \
                 --series "https://manhwaus.net/webtoon/nano-machine/" \
                 --chapters 2 --format cbz \
                 [--separate] [--start K] [--out DIR] [--seed S] [--timeout MS]
```

- `--config FILE`  site descriptor (see `sites/manhwaus.conf`)
- `--series URL`   the series page URL
- `--chapters N`   how many chapters to download (default 1)
- `--format FMT`   output container (default `loose`; see the table below)
- `--separate`     one archive per chapter instead of one combined archive
- `--start K`      skip the first K chapters (default 0)
- `--out DIR`      output root (default `downloads/`)

Pack mode (re-package a folder, no network):

```sh
./build/media_dl --pack downloads/nano-machine/nano-machine-1-2 --format jof
```

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
| `jof` | per-page native JOF tile atlas via the firmware `ra8_jof` producer -- a full-width column (`tile_w == width`) the `ra8_longstrip` engine opens directly | nothing |
| `rabook` | build a CBZ, then `tools/epub_compile/cbz_compile.py` -> the RBKC `.rabook` | `python3` + Pillow |

`cbz`/`cbt`/`cbt.gz`/`epub`/`jof` are fully self-contained (in-tree/vendored
code, no system library or external process): `epub` is hand-built with miniz,
and `jof` reuses the firmware's own `ra8_jof_produce` host-side, so a
`.jof` the CLI writes is byte-identical to one the RA8 produces (webtoon
column). `cbt.xz` / `cbr` / `rabook` are optional -- they shell out to `xz` /
`rar` / `python3` only when producing that format and report clearly if the tool
is absent (RAR and an xz *encoder* have no small in-tree option; the RBKC
container has no C writer, so rabook uses the desktop python emitter). The
`.gz`/`.xz` variants wrap a whole tar and are opened on-device by
`ra8_comic_open_wrapped`. JOF writes one `page_NNN.jof` per page into the
chapter folder (the webtoon-native form), not a single archive file.

### Which format for a webtoon / manhwa?

**Use `--format jof` for vertical-scroll webtoons.** Their pages are single
tall strips -- real chapters run 5,000-12,000 px high. The comic formats
(`cbz`/`cbt`/`cbr`/...) decode each page as one whole image through stb_image,
which is deliberately capped at **8192 px per side** (`STBI_MAX_DIMENSIONS` in
`libs/third_party/stb/stb_image_impl.c`, bounding `w*h*bpp` so the on-device
decode fits its memory budget). A strip taller than that is rejected -- by
design, not a bug. JOF exists precisely for this: it tiles the strip via the
firmware's streaming JPEG decoder (`ra8_jpeg_sw_stream`, no whole-image
allocation) and the `ra8_longstrip` engine scrolls it a viewport at a time. So a
webtoon downloaded as `cbz` will open but some tall pages won't render; the same
series as `jof` renders every page. Standard fixed-page comics (each page a
normal book-sized image) are fine as `cbz`.

Packaging is **idempotent**: only real page images (`.jpg/.jpeg/.png/.webp/.gif/
.bmp`) are ingested, so a folder that already holds this tool's own output (a
sibling `.jof`, a previous `.cbz`) or OS junk re-packages cleanly instead of
folding a non-image "page" into the archive.

### Verifying a build

`tools/media_dl/tests/integration.sh` (also `make test-integration`) is the
cross-tool end-to-end gate: it packages synthetic, non-copyright pages into
*every* format and opens each result in the native `ra8_viewer` headless,
asserting a non-blank render -- the check that catches "packages fine but the
reader can't open it". Formats whose optional tool is absent (`rar`) are
skipped, not failed.

## Status / roadmap

Working: config-driven series -> chapter list -> polite download -> combined
(or `--separate`) packaging into every reader format, verified end-to-end by
`make test-integration` and against manhwaus.net. Known limits: naive extension
naming from the URL (Content-Type/magic-byte typing is a planned fix -- pages
are named `.jpg` even when the bytes are PNG/WebP, though the readers sniff the
real magic on open), single-site descriptor format (richer TOML later), and no
proxy/Cloudflare handling. Next: the full politeness governor (per-host token
bucket, 429/503 Retry-After backoff) and Content-Type-driven page naming.
