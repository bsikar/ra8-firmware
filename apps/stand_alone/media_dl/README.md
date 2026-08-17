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

## Where the code lives

The downloader and the CLI are two different things, and they sit in two
different places:

```
apps/shared/media_dl/       the PORTABLE CORE (fetch, state, cache, export,
  inc/  src/                verify, report, pack) over fw_if_fs and the
  sites/                    injected mdl_net vtable -- plus the site
  tests/                    descriptors, which are data of the core
  CMakeLists.txt            builds and tests standalone, with no libcurl

apps/stand_alone/media_dl/  the HOST COMPOSITION ROOT: main(), the argv
  inc/  src/                grammar, the libcurl backend, host credential
  tests/                    policy, the per-mode action wiring
  CMakeLists.txt            builds the `media_dl` executable
```

The core is not inside this product because the same core is about to have a
second build form -- a loadable on-device module under `apps/threadx_modules/`
-- and a core owned by one form is a core the other form has to copy.

The layering is one-way: **`apps/shared` never includes from `apps/stand_alone`
or `apps/threadx_modules`.** That the core configures, builds and passes its own
17-binary test suite with no `find_package(CURL)` anywhere in its listfile is
the standing proof it holds.


## What it does

**series mode** (the real one): read a per-site descriptor, list a series'
chapters, download the first N, and package them into a reader-openable file.
By default the N chapters **combine into one archive** named for the chapter
range:

```
downloads/<series-slug>/<slug>-<lo>-<hi>.<ext>     e.g. my-series-1-2.cbz
```

Pass `--separate` for one archive per chapter instead, or `--format loose` (the
default when `--format` is omitted) to stop at page-image folders and skip
packaging. Continuous page numbering across chapters means the combined archive
reads as one contiguous book.

Series mode keeps **persistent library state** (a `.mdl_state` file per series),
so it is resumable and incrementally updatable rather than an index-into-a-scrape
exercise:

- `--from CHAP` starts at the chapter NUMBERED `CHAP` (a parsed chapter number,
  stable as the site adds/reorders chapters -- not a position in today's list,
  which is what the old `--start K` was).
- `--update` fetches only the chapters not already recorded complete -- the
  primary mode for following an ongoing manhwa/manga.
- An interrupted download **resumes**: a chapter is marked complete only after
  every page is fetched and its bytes verified, so the next run refetches just
  the missing pages and reproduces the same (combined) page numbering.
- A byte-identical image already held is **reused, not re-fetched** -- across
  reruns and across chapters that share an image (content-hash dedup).

**search / browse mode** (`--config S.conf --search TERM`, or `--browse`): find a
series without already knowing its URL. `--search TERM` fetches the descriptor's
`search_url` (with `{q}` replaced by the percent-encoded term), parses the result
list, and prints a numbered `title + series URL` per hit; `--browse` does the same
over the descriptor's `browse_url` (a latest-updates page). Add `--pick N` to feed
hit `N` straight into a download -- the same `--format`/`--chapters`/`--out`
options apply -- so there is no copy-paste step. Every discovery request goes
through the same politeness governor and `robots.txt` gate as a download (search is
never a rate-limit bypass), and the honesty rule holds: zero results, a
changed-markup page, and a failed request are three distinct messages, never an
empty list dressed up as success. The term is percent-encoded, so spaces, `&`,
`#`, `+` and non-ASCII (UTF-8) terms all produce a valid request.

**library mode** (over `--out`): `--list` prints every tracked series with its
chapter coverage and gaps, `--update-all --config S.conf` incrementally updates
them all, and `--remove URL|SLUG` drops one series.

**verify mode** (`--verify [DIR]`): verify existing downloaded archives and page
images in `downloads/` (or a specified directory) against recorded `.mdl_state`
content hashes and check container integrity.

**init-site wizard** (`--init-site URL`): generate a starter `.conf` site
descriptor template for a site given its URL, helping quickly add support for
new comic sites.

**pack mode** (`--pack DIR --format FMT`): package an existing folder of page
images -- no network. Handy for re-encoding a download into another format and
for the integration harness.

**page mode** (debug): fetch one URL and download its `<img>` URLs.

**direct artifact mode**: a bare HTTPS URL ending in `.cbz`, `.cbt`, `.cbt.gz`,
`.epub`, or `.jof` is streamed to a sibling staging file, structurally
validated, and only then atomically published under `--out`. A failed or
unsupported download never replaces an existing artifact.

The scope is deliberately small (unlike the half-baked Kotlin original): fetch,
extract, download politely, resumably, package.

### Command behavior matrix

Exactly one mode is required. Repeated flags/options, multiple modes, missing
values, and options that have no effect in the selected mode are usage errors
(exit 2); dispatch never relies on precedence.

| Mode | Required | Mode-specific optional arguments |
| --- | --- | --- |
| series | `--config FILE --series URL` | `--out`, `--chapters`, `--from`, `--format`, `--separate`, `--update`, `--allow-incomplete`, `--progress`, `--refetch` |
| search | `--config FILE --search TERM` | `--pick N`; download options above are accepted only with `--pick` |
| browse | `--config FILE --browse` | `--pick N`; download options above are accepted only with `--pick` |
| list | `--list` | `--out` |
| update all | `--update-all --config FILE` | `--out`, `--format`, `--separate`, `--allow-incomplete`, `--progress`, `--refetch` |
| remove | `--remove URL\|SLUG` | `--out`; removal refuses directories without a valid tracked-state marker |
| verify | `--verify [DIR]` | `--out DIR` is an alternate spelling; using both is an error |
| init site | `--init-site URL` | none |
| pack | `--pack DIR --format FMT` | none |
| direct artifact | bare HTTPS artifact URL | `--out` plus network/security controls; structurally verified formats are `cbz`, `cbt`, `cbt.gz`, `epub`, `jof` |
| page debug | bare `URL` | `--out`, `--max`, `--attr src\|data-src` |

Network modes additionally accept the applicable identity/security controls:
`--seed`, `--timeout`, `--contact`, `--max-bytes`, `--proxy` or `--socks5`,
`--cookie-file`, `--polite`, `--ignore-robots`, `--allow-private`, and
`--cross-host`. `--ca-file FILE` selects a custom PEM trust bundle for private
or test HTTPS endpoints without disabling peer or hostname verification. Proxy
modes require `--allow-private` because libcurl's proxy
peer callback cannot prove the proxy's target address is public.

`--verify` proves tracked page hashes and structurally parses every recognized
artifact. CBZ/EPUB ZIP members (including CRC), CBT/CBT.GZ tar headers, gzip
CRC/size, and JOF tables are validated in process. CBR, CBT.XZ, and rabook are
not verification targets and are never presented as verified.
`.INCOMPLETE` names and the multi-dot CBT.GZ suffix are recognized from the
complete filename.

## Design (why it maps cleanly to the RA8 later)

- `mdl_net.{h,c}` -- streaming HTTP GET **seam**, a real function-pointer vtable
  mirroring the on-device `ra8_ota_net_iface_t`. `mdl_net.h` defines the
  `mdl_net_vtable_t` (get-to-buffer, get-to-file, last-status, destroy) and the
  `{ vtable, ctx }` handle; `mdl_net.c` holds the backend-agnostic dispatchers
  and their argument validation. Callers reach a backend only through the
  dispatchers, so swapping backends -- or a scripted mock in the host tests --
  is a vtable substitution, never a relink (NASA Rule 9 DIP deviation).
- `mdl_net_curl.{h,c}` -- the concrete libcurl backend, registered through the
  seam. `mdl_net_curl.h` (included only by the composition root, `main.c`) is
  the ONE place that names the backend; every other layer sees only `mdl_net.h`.
  libcurl gives TLS, redirects, gzip and one reused connection with a persistent
  cookie jar. On the RA8 this becomes NetX Duo + Mbed TLS over the C6; callers do
  not change. The backend is hardened for attacker-controlled URLs: transport
  pinned to http/https, redirects refused when they change host or resolve to
  loopback/private/link-local address space (the SSRF guard, on the resolved
  peer), explicit TLS verification, `.netrc` and proxy-env disabled, and a
  per-response size + low-speed cap.
- `mdl_url_guard.{h,c}` -- pure URL/address predicates the backend enforces
  (scheme allowlist, IP classification, size cap, host/path extraction).
- `mdl_sanitize.{h,c}` -- neutralise untrusted names before a filesystem or XML
  sink: segment sanitiser (`..`/reserved/over-length), path containment, and an
  XML escaper for the generated EPUB.
- `mdl_robots.{h,c}` -- robots.txt parser (most-specific `User-agent` group,
  longest-match `Allow`/`Disallow`, `Crawl-delay`) + a per-host cache.
- `mdl_session.{h,c}` -- honest configurable User-Agent + robots.txt gating.
- `mdl_cli.{h,c}` -- command-line parsing.
- `mdl_extract.{h,c}` -- `<img>`/`<a>` tag scanner + relative-URL resolver.
  Replaced on-device by litehtml (already vendored) behind the same signatures.
- `mdl_config.{h,c}` -- strict flat key=value **site descriptor** loader. Adding
  a site is dropping a `.conf` in `sites/`, no rebuild. Descriptors carry
  chapter/image/discovery rules plus bounded series/chapter metadata selectors,
  language, and reading direction. Unknown keys, empty/malformed selectors,
  overlong lines/values, invalid numeric ranges, and contradictory bounds fail
  configuration instead of being ignored. The fixed-size struct performs no
  dynamic allocation.
- `mdl_state.{h,c}` -- **persistent per-series library state**: a versioned,
  flat, TAB-separated schema-v2 `.mdl_state` file recording the series identity,
  title, synopsis, writer, artist, verified local/remote cover identity,
  language/direction, descriptor used, and -- per chapter -- title, explicit
  fractional-or-missing number, source URL, page count, completion status and
  fetch time, plus a series-wide pool of per-page content identities. Version 1
  state is migrated in memory and written as v2. Writes are atomic and durable;
  corrupt state fails closed and is never silently overwritten. Fixed-size
  struct, zero dynamic allocation.
- `mdl_hash.{h,c}` -- FNV-1a 64 content-identity hashing (buffer, string, and a
  streamed file), used to key the URL dedup lookup and to verify a page on disk.
- `mdl_urlname.{h,c}` -- pure URL-to-name helpers (sanitised last segment ->
  chapter identifier / slug, last-digit-run chapter number, page extension).
- `mdl_fetch.{h,c}` -- the state-aware download **orchestrator**: resumes
  interrupted chapters page-wise, skips complete chapters under `--update`,
  dedups already-held pages by content, and derives combined page numbers from
  recorded per-chapter counts. Driven end-to-end through the `mdl_net` mock in
  the host tests (first run fetches N; second fetches only the new ones;
  interrupted run resumes byte-identical).
- `mdl_library.{h,c}` -- library-wide walk over a directory of tracked series
  (`--list`/`--update-all`) and one-series tree removal (`--remove`).
- `mdl_politeness.{h,c}` -- seeded, jittered inter-request delay plus the global
  per-host token-bucket governor, adaptive transport/429/5xx backoff, and both
  delta-seconds and HTTP-date `Retry-After`. The clock/sleep seams are injectable,
  so spacing, decay, saturation, and wall-clock conversion are deterministic in
  unit tests.
- Return type is `ra8_err_t` from `libs/ra8_core` -- signatures are already
  device-shaped.

Intentional fixes vs. the Kotlin original: a truthful, configurable User-Agent
held constant per session (not a spoofed browser string, and not rotated per
request -- rotation reads as *more* bot-like), one reused HTTP connection
(shared pool + cookies), and image extraction that prefers `data-src`
(lazy-loaded) with a URL-substring filter to drop loader/nav/ad images. Set your
contact with `--contact <email|url>` (or a `contact =` key in the site
descriptor) so an operator can reach you rather than ban a netblock.

robots.txt is honoured by default: `/robots.txt` is fetched once per host, the
most specific `User-agent` group is applied, a disallowed URL is refused before
any request, and a `Crawl-delay` raises the per-host politeness floor. `--polite`
raises delays further, `--ignore-robots` is a loud, explicit escape hatch, and
`--allow-private` / `--cross-host` open the SSRF guards only when asked.

## Build

```sh
cd apps/stand_alone/media_dl
cmake -B build -S .
cmake --build build -j
```

Requires libcurl (present on macOS by default) and a C23 host compiler
(Apple clang 21 works). Nothing here is cross-compiled to ARM.

## Run

Series mode (combined by default -- two chapters into one `my-series-1-2.cbz`):

```sh
./build/media_dl --config sites/manhwaus.conf \
                 --series "https://example.com/webtoon/my-series/" \
                 --chapters 2 --format cbz \
                 [--separate] [--from CHAP] [--update] [--out DIR] [--seed S] [--timeout MS]
```

- `--config FILE`  site descriptor (see `sites/manhwaus.conf`)
- `--series URL`   the series page URL
- `--chapters N`   how many chapters to download (default 1)
- `--format FMT`   output container (default `loose`; see the table below)
- `--separate`     one archive per chapter instead of one combined archive
- `--from CHAP`    start at the chapter NUMBERED `CHAP` (a chapter number, not an
                   index into the scraped list); download up to `--chapters` of them
- `--update`       fetch only chapters not already recorded complete (incremental)
- `--out DIR`      output root / library (default `downloads/`)
- `--progress`     terminal progress bar during downloads
- `--proxy URL`    HTTP/HTTPS proxy URL passed to libcurl
- `--socks5 URL`   SOCKS5 proxy URL passed to libcurl
- `--cookie-file FILE` pass cookies file to libcurl
- `--verify [DIR]` verify existing downloaded archives/files in `downloads/`
- `--init-site URL` helper wizard that generates a starter `.conf` site descriptor template

Following a series is then: run once, and later `--update` (or `--update-all`)
to pull whatever is new. Re-running never re-downloads a page already held, and a
run killed part-way resumes where it stopped.

Library commands (over `--out`):

```sh
./build/media_dl --list                                # tracked series + coverage/gaps
./build/media_dl --update-all --config sites/S.conf    # incremental update of all series
./build/media_dl --remove "https://site/webtoon/foo/"  # drop one series (URL or slug)
```

### Library state (`.mdl_state`)

Each series directory holds a `.mdl_state` file: a versioned, flat,
TAB-separated, `#`-commented text record. Schema v2 uses `S/T/N/H/G` for
identity/descriptor, `D/W/A/O/K/L/R` for rich series metadata, one `C` line per
chapter (including title and an explicit known fractional number), and one `P`
line per fetched page carrying its source-URL hash, content hash, relative path,
ETag, and Last-Modified value. It is written atomically and durably. A v1 file
is migrated; a malformed file causes the command to fail closed so it cannot
erase or silently replace the last known-good state.

Pack mode (re-package a folder, no network):

```sh
./build/media_dl --pack downloads/my-series/my-series-1-2 --format jof
```

Page mode (debug):

```sh
./build/media_dl <page-url> [--out DIR] [--max N] [--attr data-src|src]
```

## Site descriptors

A `.conf` is flat `key = value` (`#` comments, `[section]` lines ignored). The
qualified descriptors are `sites/manhwaus.conf` and
`sites/peppercarrot.conf`, each backed by captured series/chapter fixtures.
Core keys are `name`, `host`, `kind`, `chapter_url_contains`, optional
`chapter_url_prefix`, `chapter_order` (`asc`|`reverse`|`doc`),
`page_img_attr`, `page_img_url_contains`, and the `*_delay_min/max` governor
bounds. Discovery uses `search_url` (one `{q}` placeholder),
`search_result_contains`, and `browse_url`.

Metadata selectors use one of four bounded forms: `meta:og:title` reads a meta
content value, `class:post-title` reads cleaned element text, `label:Author(s):`
reads the value following a visible label, and `literal:David Revoy` supplies a
descriptor-owned constant. The `series_title_selector`,
`series_summary_selector`, `series_author_selector`, `series_artist_selector`,
`series_cover_selector`, and `chapter_title_selector` fields use that grammar;
`language` is a BCP-47 tag and `reading_direction` is `ltr` or `rtl`. Cover URLs
are resolved against the series page, fetched through the normal policy path,
typed by byte signature, persisted locally, and embedded canonically in CBZ and
EPUB output. A descriptor without search/browse simply reports that mode as
unavailable. Sites requiring a JavaScript challenge are not claimed supported.

## Export formats (`--format`)

| `--format` | How it's made | Needs |
|------------|---------------|-------|
| `loose` (default) | page images in a folder | nothing |
| `cbz` | vendored miniz ZIP writer (STORE) | nothing |
| `cbt` | hand-written POSIX ustar tar | nothing |
| `cbt.gz` | miniz DEFLATE + RFC-1952 gzip framing | nothing |
| `epub` | a valid EPUB3 of the pages via vendored miniz (`ra8_epub` opens it) | nothing |
| `jof` | per-page native JOF tile atlas via the firmware `ra8_jof` producer -- a full-width column (`tile_w == width`) the `ra8_longstrip` engine opens directly | nothing |
| `rabook` | strict reader-native RABOOK1 data in an independently compressed RBKC container | nothing |

`cbz`/`cbt`/`cbt.gz`/`epub`/`jof`/`rabook` are fully self-contained (in-tree/vendored
code, no system library or external process): `epub` is hand-built with miniz,
and `jof` reuses the firmware's own `ra8_jof_produce` host-side, so a
`.jof` the CLI writes is byte-identical to one the RA8 produces (webtoon
column). RABOOK export builds an EPUB-compatible page source, compiles it to
the native RABOOK1 model, writes the chunked RBKC container, strictly validates
the staged bytes, and only then publishes the `.rabook` file. CBR and CBT.XZ
enums remain for reader compatibility but are not accepted as host CLI export
formats. The `.gz` variant wraps a whole tar and is opened on-device by
`ra8_comic_open_wrapped`. JOF writes one
`page_NNN.jof` per page into the
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

The CTest suite covers strict CLI conjunctions, both site descriptors and their
captured shapes, schema-v1 migration/schema-v2 round trips, resume/revalidation,
failure propagation, every advertised exporter and structural verifier, and a
real loopback HTTP/HTTPS run. The HTTPS fixture creates an ephemeral CA and
proves that an untrusted server fails, `--ca-file` succeeds without disabling
verification, corrupt direct artifacts are not published, rich metadata and a
magic-typed cover reach a real CBZ, and fractional plus numberless chapters
retain document order. Hardware reader-open qualification is intentionally not
claimed here; it remains pending while hardware use is on hold.

## Status / roadmap

Working and host-qualified: strict descriptor-driven series/discovery commands;
fractional chapter selection; rich series/chapter metadata and covers;
polite/resumable/revalidated downloads; persistent library operations; direct
verified HTTPS artifacts; and bounded export/verification for CBZ, CBT, CBT.GZ,
EPUB, JOF, and RABOOK. Page and cover names are selected from content bytes
rather than trusted URL suffixes. ManhwaUS and Pepper&Carrot have descriptor
fixtures; live sites can still change independently and therefore remain
observable runtime dependencies, not permanent guarantees.

Not yet claimed complete: CBR and CBT.XZ host export; a fixed-memory arbitrary-URL
HTTPS implementation on the ESP32-C6 (ESP-IDF's HTTP/TLS stack allocates
transitively); full scrape/package orchestration across the two processors; and
real hardware reader/RPC qualification. The protobuf RPC coordinator and model
tests are real, but the current C6 path transfers a prebuilt artifact rather
than implementing the full URL-to-RABOOK workflow promised by issue #665. Those
boundaries return or are documented as unsupported instead of masquerading as
completed functionality.
