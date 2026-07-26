# SOUP Justification: stb (image / truetype)

Per IEC 61508-3 Section 7.4.2.12 and DO-178C Section 12.1.4, this document
records the qualification basis for accepting the stb single-file
libraries into this firmware as Software Of Unknown Provenance (SOUP).

## Component identity

- **Name**: stb_image and stb_truetype (single-file public-domain
  libraries)
- **Version**:
  - `stb_image.h` v2.30 (per file header).
  - `stb_truetype.h` v1.26 (per file header).
- **Upstream URL**: https://github.com/nothings/stb
- **Local path**: `libs/third_party/stb/`
  - Files in tree: `stb_image.h`, `stb_truetype.h`,
    `stb_truetype_impl.c`.

## Provenance

- **Origin**: Sean Barrett (nothings.org) / RAD Game Tools.
- **License**: Public domain (per file headers); the upstream `stb`
  repo dual-publishes under MIT for jurisdictions that do not recognize
  public domain. No `LICENSE` file is shipped in our subdirectory; the
  in-file headers carry the terms.
- **How it entered our tree**: Vendored individual headers / impl files
  from the upstream stb repository. Upstream commit hash unknown.

## Use case in this firmware

- `stb_image`: PNG / JPEG decoding for cover art and inline EPUB images
  inside `libs/ra8_epub/` and `libs/ra8_gfx/`.
- `stb_truetype`: TTF font rasterization for the EPUB reader's text
  layout path.
- Integrity claim category: data-handling (decoders consume locally
  staged image / font payloads).

## Qualification basis

Accepted as-is per IEC 61508-3 Section 7.4.2.12 and DO-178C Section
12.1.4:

- **Service history**: stb single-file libraries are among the most
  widely deployed C libraries in industry, used by major game engines
  and tools since 2009.
- **Open-source community process**: Open GitHub project, large
  community auditing.
- **Bug tracker review**: Issues at https://github.com/nothings/stb/issues
  reviewed. The `stb_truetype.h` header explicitly states "NO SECURITY
  GUARANTEE -- DO NOT USE THIS ON UNTRUSTED FONT FILES". The e-reader
  DOES feed it fully attacker-controlled fonts (EPUB `@font-face`
  resources and book fonts), so it cannot rely on trusted input.
  `stb_image.h` is likewise reached directly by attacker-controlled
  EPUB/CBZ images. Both are hardened against out-of-bounds reads as
  recorded under "Deviations / patches" below.

## Risk mitigation

- Both decoders run only on locally staged content (EPUB payloads);
  there is no network-driven decoding path.
- Output buffer sizes are bounded by the framebuffer dimensions in
  `libs/ra8_gfx/`.

## Deviations / patches

`stb_image.h` is unmodified.

`stb_truetype.h` carries local memory-safety hardening. Upstream
stb_truetype performs no bounds checking on the offsets it reads out of
the font file, so a crafted TrueType/OpenType font drives out-of-bounds
reads once parsing moves past the sfnt table directory. The e-reader
feeds it fully attacker-controlled fonts, so those reads are an
initial-access memory-safety hole (surfaced by the `fuzz_ra8_stbtt`
libFuzzer harness under AddressSanitizer). The vendored file is patched
in place; every change is delimited by an `RA8 LOCAL PATCH` comment that
back-references this document. The patch is minimal and mechanical -- it
adds bounds and allocation-failure checks, it does not restructure the
parser:

- **`stbtt_fontinfo.data_size`**: new field recording the byte length of
  the font buffer. `stbtt_InitFont()` / `stbtt_InitFont_internal()` gain
  a `data_size` parameter so the length is available to every read; all
  first-party call sites pass the true length (three dead upstream
  convenience APIs -- `stbtt_BakeFontBitmap`, `stbtt_PackFontRanges`,
  `stbtt_GetScaledFontVMetrics`, none used by this firmware -- carry no
  length and are left at upstream behaviour via `INT_MAX`).
- **`stbtt__in_bounds()`**: new helper; `[offset, offset+count)` must lie
  inside `[0, data_size)`, computed without wrapping arithmetic.
- **`stbtt_InitFont_internal()`**: bounds the `maxp`, `head`, `hhea` and
  cmap-subtable-directory reads.
- **`stbtt_FindGlyphIndex()`**: bounds the `index_map` subtable header and
  each cmap format arm (0 / 4 / 6 / 12 / 13), including the format-4
  binary-search reads and the attacker-`idRangeOffset`-driven glyph-id
  read.
- **`stbtt__GetGlyfOffset()`**: rejects a negative glyph index, bounds the
  `loca` read, and requires the resolved glyph header to lie inside the
  buffer.
- **`stbtt__GetGlyphShapeTT()`**: bounds the `endPtsOfContours` /
  instruction-length reads and guards every byte of the simple-glyph
  point stream and the composite-component walk against the buffer end;
  gains a recursion-depth cap (`STBTT__MAX_COMPOSITE_DEPTH`) so a
  self-referencing composite cannot overflow the stack.
- **CFF/OTF path**: the CFF `stbtt__buf` is sized to the real remaining
  bytes instead of the upstream bogus 512 MB, so the built-in `stbtt__buf`
  bounds checks fire on a crafted OTF.
- **`stbtt__rasterize_sorted_edges()`**: null-checks the per-scanline
  `STBTT_malloc`. `STBTT_malloc` is wired to the fixed bump arena
  `ra8_stbtt_malloc` (`libs/ra8_reflow/src/ra8_stbtt_alloc.c`), which returns
  NULL on exhaustion by design (NASA P10 Rule 3, zero dynamic allocation after
  init). Upstream stb never checks this allocation, so a crafted glyph that is
  wide (`result->w > 64`, taking the heap scanline rather than the 129-float
  stack buffer) AND carries a huge edge list -- draining the arena in the
  preceding edge-list allocation -- left `scanline == NULL`, and the following
  `STBTT_memset(scanline, ...)` wrote to a NULL pointer (an AddressSanitizer
  SEGV surfaced by `fuzz_ra8_stbtt` inside `stbtt__rasterize_sorted_edges`; the
  same `stbtt_MakeCodepointBitmap` path is reached by the real reader in
  `ra8_epub_chapter.c` `priv_render_into`). The patch returns early on the NULL,
  rendering that glyph blank -- the same graceful degradation the arena already
  yields elsewhere on exhaustion. The other allocations on this rasterisation
  path already null-check upstream: the edge list (`stbtt__rasterize`), the
  active-edge hheap chunk (`stbtt__hheap_alloc` / `stbtt__new_active`), and the
  `stbtt_FlattenCurves` point / contour-length buffers. The identical guard is
  mirrored in the `STBTT_RASTERIZER_VERSION==1` rasteriser for consistency,
  though the firmware builds version 2.

On any out-of-bounds condition the font (or the individual glyph) is
rejected cleanly -- an empty glyph or an init/lookup failure -- never a
read past the buffer; on arena exhaustion the glyph is likewise rendered
blank rather than dereferencing a NULL allocation. Regression reproducers are
committed under `tests/fuzz/corpus/fuzz_ra8_stbtt/crash-*`: the out-of-bounds
`loca` case is additionally hand-minimised into `tests/test_ra8_stbtt_guard.c`,
and the arena-exhaustion NULL-deref case is replayed on every `fuzz_ra8_stbtt`
sweep (`scripts/checks/run_fuzz.sh`), so a regression re-aborts under
AddressSanitizer.

## Last review date

- Reviewed: 2026-05-02
- stb_truetype.h memory-safety hardening: 2026-07-15
- stb_truetype.h rasteriser arena-exhaustion NULL-deref guard: 2026-07-25
- Expected re-review by: 2027-05-02
