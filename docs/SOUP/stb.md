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
  inside `libs/ra_epub/` and `libs/ra_gfx/`.
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
  GUARANTEE -- DO NOT USE THIS ON UNTRUSTED FONT FILES"; we comply by
  only feeding it fonts that ship inside our trusted EPUB payloads.

## Risk mitigation

- Both decoders run only on locally staged content (EPUB payloads);
  there is no network-driven decoding path.
- Output buffer sizes are bounded by the framebuffer dimensions in
  `libs/ra_gfx/`.

## Deviations / patches

None. The vendored headers are unmodified.

## Last review date

- Reviewed: 2026-05-02
- Expected re-review by: 2027-05-02
