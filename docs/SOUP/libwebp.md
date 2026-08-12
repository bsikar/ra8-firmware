# SOUP Justification: libwebp (WebP decoder)

Per IEC 61508-3 Section 7.4.2.12 and DO-178C Section 12.1.4, this document
records the qualification basis for accepting the Google libwebp decoder
into this firmware as Software Of Unknown Provenance (SOUP).

## Component identity

- **Name**: libwebp (WebP image codec) -- **decode-only** subset.
- **Version**: 1.5.0 (release tag `v1.5.0`).
- **Upstream URL**: https://chromium.googlesource.com/webm/libwebp
  (mirror: https://github.com/webmproject/libwebp)
- **Local path**: `libs/third_party/libwebp/`

## Provenance

- **Origin**: Google Inc. / the WebM Project.
- **License**: BSD-3-Clause (`COPYING`) plus an additional patent grant
  (`PATENTS`). Both files are mirrored verbatim into the vendored tree; the
  contributor list is mirrored as `AUTHORS`.
- **How it entered our tree (commit pin)**: the vendored files are
  byte-identical to upstream release tag **`v1.5.0`**, commit
  `a4d7a715337ded4451fec90ff8ce79728e04126c`. Only the WebP **decoder**
  translation units are vendored -- the exact source set of upstream's
  `libwebpdecoder` static library (`src/dec/`, the decode subset of
  `src/dsp/`, and `src/utils/` COMMON sources) plus the public + internal
  headers those TUs include (`src/webp/`, `src/dec/`, `src/dsp/`, `src/utils/`,
  and the two `src/enc/` headers -- `histogram_enc.h`, `backward_references_enc.h`
  -- that the shared `src/dsp/lossless.h` transitively pulls in). The encoder
  (`src/enc/*.c`), the muxer/demuxer (`src/mux/`, `src/demux/`), `sharpyuv/`,
  the CLI tools (`examples/`, `imageio/`) and `extras/` are **not** vendored
  ("configure out the encoder"). One TU is modified; see
  "Deviations / patches" below.

## Use case in this firmware

- **WebP decode** (VP8 lossy + VP8L lossless) for longstrip / manga / EPUB
  raster content, which is increasingly shipped as WebP that `stb_image`
  cannot decode. Reached through the first-party `ra8_webp` facade
  (`libs/ra8_webp/`) -- `ra8_webp_get_info()` / `ra8_webp_decode_rgba()` --
  never through libwebp's API directly.
- Integrity claim category: data-handling. The decoder consumes fully
  attacker-controlled initial-access content (a WebP inside a downloaded
  book), so it cannot rely on trusted input.
- **Scope note -- where the decoder is wired, and where it is not.** The
  decoder is vendored, built, standalone-tested and fuzzed (that was #290).
  Render-time decodes reach it through the band-tile producer:
  `libs/ra8_jof/src/ra8_jof_produce_webp.c` normalises a decoded WebP into the
  band-tile format, so comic and EPUB **tiles** take WebP. That is the half of
  #289 that landed before it closed on 2026-07-20. The other half did not: the
  `ra8_reflow` **inline small-image** path
  (`libs/ra8_reflow/src/ra8_reflow_image.c`) is still `stb_image`-only and
  fails a WebP closed. That residual arm is tracked by #637, which also owns
  the `TODO(#289)` seam comments left in `ra8_webp.c` / `ra8_webp.h`.

## Qualification basis

Accepted as-is per IEC 61508-3 Section 7.4.2.12 and DO-178C Section 12.1.4:

- **Service history**: libwebp is the reference WebP implementation, shipped
  in every major browser and OS image-decode stack since 2010 and used at
  internet scale.
- **Open-source community process**: active upstream (Google/WebM), broad
  external review.
- **Continuous fuzzing**: libwebp is under continuous OSS-Fuzz coverage
  upstream. This repository additionally fuzzes the vendored decoder through
  the exact firmware entry point (`tests/fuzz/fuzz_ra8_webp.c`, libFuzzer +
  UBSan, ASan on Linux) so a regression on our pinned copy is caught here.
- **Bug tracker review**: the vendored version is the current `v1.5.0`
  release, which carries the fix for **CVE-2023-4863** (the 2023 VP8L
  Huffman-table heap overflow) and every advisory resolved before the 1.5.0
  tag. Re-check the upstream security advisories at the next re-vendor.

## Risk mitigation

- The decoder runs only on locally staged content (book payloads); there is
  no network-driven decode path.
- The `ra8_webp` facade bounds the declared dimensions
  (`k_ra8_webp_max_dim = 8192` per axis) and the output buffer size before
  committing to a decode, so a malicious header cannot demand a
  multi-gigapixel buffer or overflow the RGBA output sizing.
- Output is decoded directly into a caller-provided, bounded RGBA8888 buffer
  (`WebPDecodeRGBAInto`); the decoder never allocates the output.
- The libFuzzer harness under ASan/UBSan is the primary memory-safety net for
  this attacker-facing decoder (the vendored TUs are compiled `-w`; see the
  build note below).

## Deviations / patches

**Decode-only subset**: only the decoder TUs are vendored (see "Provenance").
This is a subset selection, not a source modification -- every vendored file
is byte-identical to `v1.5.0`, with the one exception below.

**`src/utils/utils.c` -- heap-free allocator (MODIFIED SOUP).** The firmware
has no heap (`_sbrk` traps after init, NASA Power-of-10 Rule 3). libwebp
funnels every allocation through `WebPSafeMalloc` / `WebPSafeCalloc` /
`WebPSafeFree` in this file. The vendored copy is patched **in place**, gated
by the build define `-DRA8_WEBP_USE_ARENA`, so those three functions call the
first-party `ra8_webp_arena_*` bump allocator (`libs/ra8_webp/`) instead of
libc `malloc` / `calloc` / `free`. Every changed line is delimited by an
`RA8 LOCAL PATCH` comment that back-references this document; the patch is
minimal and mechanical (it swaps the allocator call, it does not restructure
the parser), and without the define the unmodified libc path is used (for host
tooling with no arena bound). This mirrors how `stb_image` is fronted by
`ra8_img_arena` (`libs/third_party/stb/stb_image_impl.c`); a **separate**
sibling arena is used rather than reusing `ra8_img_arena` to keep the WebP
decoder decoupled from `libs/ra8_reflow` (still true: see #637), and because
the WebP path additionally needs a zeroing `ra8_webp_arena_calloc`. The arena is a
reference-counted bump allocator that fully drains after each decode; on
exhaustion the hook returns `NULL` and libwebp propagates a clean decode
failure. On-target proof: `examples/.../webp_decode_demo` links **no** libc
`malloc` / `calloc` / `free` -- `WebPSafe*` resolve to `ra8_webp_arena_*`.

**Build flags (not source edits).** The vendored decoder is compiled with a
blanket `-w -fno-strict-aliasing` (plus `-DRA8_WEBP_USE_ARENA`), applied by
`ra8_webp_apply_soup_flags()` in `cmake/ra8_webp_vendor.cmake` -- the one
module that owns the vendored-source recipe, called from
`cmake/ra8_app/vendored.cmake` for firmware apps and from
`tests/cmake/core_hal.cmake` for the host tests. (It lived inline in
`tests/CMakeLists.txt` and `cmake/ra8_add_app.cmake` until `1171d656d`; neither
file mentions WebP now.) Unlike the narrow `-Wno-<class>` set used for
`stb` / `miniz` (issue #179), per-TU warning tuning is not tractable across
libwebp's 60+ decoder + per-arch SIMD-stub TUs; the libFuzzer/ASan/UBSan
harness is the memory-safety net instead. `-fno-strict-aliasing` matches the
project's SOUP-decoder policy (these codecs type-pun through byte buffers).
No `HAVE_CONFIG_H` / `config.h` is supplied, so the portable C reference paths
are used on the Cortex-M85 target (no NEON/SSE on the M85) and the per-arch
SIMD TUs compile to empty objects there; the host build uses the CPU's
baseline SIMD, which is bit-identical for the lossless (VP8L) path.

## Last review date

- Reviewed: 2026-07-15
- Vendored at upstream `v1.5.0` (`a4d7a715`).
- Build-flag location and wiring scope re-verified against the tree and
  corrected (#617): 2026-08-04.
- Expected re-review by: 2027-07-15
