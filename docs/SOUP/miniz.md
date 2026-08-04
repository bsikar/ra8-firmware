# SOUP Justification: miniz

Per IEC 61508-3 Section 7.4.2.12 and DO-178C Section 12.1.4, this document
records the qualification basis for accepting miniz into this firmware as
Software Of Unknown Provenance (SOUP).

## Component identity

- **Name**: miniz (single-file deflate / inflate / zip)
- **Version**: 11.0.2 (per `miniz.h` MZ_VERSION = "11.0.2",
  MZ_VER_MAJOR = 11, MZ_VER_MINOR = 2, MZ_VER_REVISION = 0). That is
  upstream's INTERNAL version macro and it matches no upstream tag; the
  release this tree is pinned to is the artifact `miniz-3.0.2.zip` (see
  "Provenance"). The two numbering schemes are unrelated, which has already
  misled one audit.
- **Upstream URL**: https://github.com/richgel999/miniz
- **Local path**: `libs/third_party/miniz/`

## Provenance

- **Origin**: Rich Geldreich and Tenacious Software / RAD Game Tools.
- **License**: MIT (`LICENSE`, "Copyright 2013-2014 RAD Game Tools and
  Valve Software / Copyright 2010-2014 Rich Geldreich and Tenacious
  Software LLC").
- **How it entered our tree**: Vendored amalgamation drop-in (`miniz.c`
  + `miniz.h`). The amalgamation is published only as a RELEASE ARTIFACT
  and never existed in the upstream git tree, so it is pinned by artifact
  rather than by commit (#548): `miniz-3.0.2.zip`, SHA-256
  `ada38db0b703a56d3dd6d57bf84a9c5d664921d870d8fea4db153979fb5332c5`. All
  three vendored files are byte-identical to members of that archive.

## Use case in this firmware

Deflate / inflate / ZIP container support. Five first-party libraries decode
through it, not one:

- `libs/ra8_epub/` -- the EPUB unpacker (EPUB files are ZIP archives), via the
  `mz_zip` reader.
- `libs/ra8_comic/src/ra8_comic_cbz.c` -- CBZ comic archives, via the same
  `mz_zip` reader.
- `libs/ra8_jof/src/ra8_jof_png.c` -- PNG image data, via `tinfl`.
- `libs/ra8_unarch/src/ra8_unarch_gzip.c` -- gzip streams, via `tinfl` plus
  `mz_crc32`.
- `libs/ra8_io/src/ra8_io_compress.c` and `ra8_io_vfs_compress.c` -- the
  compress-on-write / decompress-on-read fabric seam that RBKC chunks ride.

The host tools `tools/media_dl` (`mz_zip_writer` + `tdefl`, the one
COMPRESSION consumer, run on downloaded media) and `tools/cache_bench` also
compile it.

- Integrity claim category: data-handling (decompression of locally staged,
  attacker-authored book and archive payloads -- a book is authored by whoever
  made it, so the bytes are hostile even though the transport is not).

## Qualification basis

Accepted as-is per IEC 61508-3 Section 7.4.2.12 and DO-178C Section
12.1.4:

- **Service history**: miniz has shipped as the single-file compression
  drop-in in countless game and tool projects since 2010.
- **Open-source community process**: Open GitHub project with public
  issue tracker.
- **Bug tracker review**: Issues at
  https://github.com/richgel999/miniz/issues reviewed; no open
  advisories at the 11.0.x release line affect the decode paths used here
  (`mz_zip` reader, `tinfl`, `mz_crc32`).

## Risk mitigation

- Every firmware consumer decodes locally staged content (SD card, MRAM,
  Octo-SPI); no network payload feeds a decoder on the target. The one
  network-fed consumer, `tools/media_dl`, is a host tool and is not part of
  the firmware image.
- Decompression limits are charged across the whole surface rather than in the
  EPUB wrapper alone: `libs/ra8_epub/src/ra8_epub_zip_guard.c` is the
  decompression-limits retrofit for every miniz ZIP consumer, and it plus
  `ra8_comic_cbz.c` and `ra8_unarch_gzip.c` charge the unified policy in
  `libs/ra8_core/inc/ra8_decomp_limits.h` -- per-unit output cap,
  compression-ratio bound (decompression bombs), and a decode-loop iteration
  budget.
- Allocation is bounded and heap-free: miniz allocates from the 160 KiB static
  pool declared in `libs/ra8_epub/inc/ra8_epub_miniz_alloc.h`
  (`k_ra8_epub_miniz_pool_bytes`), because this firmware traps `_sbrk` (NASA
  P10 Rule 3).
- Fuzzed: `tests/fuzz/fuzz_ra8_epub.c` and `tests/fuzz/fuzz_ra8_unarch_gzip.c`
  drive hostile archives through the first-party wrappers under ASan/UBSan.

## Deviations / patches

None: all three vendored files are byte-identical to members of the pinned
`miniz-3.0.2.zip` release artifact, verified on every CI run against
`docs/sbom/upstream/miniz.manifest`.

They were not, until #548. The vendor-in sweep (`75b635cc7`) ran the project
formatter over the amalgamation, so `miniz.c` and `miniz.h` differed from the
published bytes by macro-continuation and pointer-style re-spacing throughout
-- semantically identical, and a complete break of the byte-identity claim this
document makes. Both files were restored to the release artifact's bytes.

## Last review date

- Reviewed: 2026-05-02
- Use case + risk mitigation re-verified against the tree and corrected
  (#620): 2026-08-04. The qualification had been scoped to EPUB alone while
  five first-party libraries decode through this component.
- Expected re-review by: 2027-05-02
