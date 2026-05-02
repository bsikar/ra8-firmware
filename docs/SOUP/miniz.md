# SOUP Justification: miniz

Per IEC 61508-3 Section 7.4.2.12 and DO-178C Section 12.1.4, this document
records the qualification basis for accepting miniz into this firmware as
Software Of Unknown Provenance (SOUP).

## Component identity

- **Name**: miniz (single-file deflate / inflate / zip)
- **Version**: 11.0.2 (per `miniz.h` MZ_VERSION = "11.0.2",
  MZ_VER_MAJOR = 11, MZ_VER_MINOR = 2, MZ_VER_REVISION = 0).
- **Upstream URL**: https://github.com/richgel999/miniz
- **Local path**: `libs/third_party/miniz/`

## Provenance

- **Origin**: Rich Geldreich and Tenacious Software / RAD Game Tools.
- **License**: MIT (`LICENSE`, "Copyright 2013-2014 RAD Game Tools and
  Valve Software / Copyright 2010-2014 Rich Geldreich and Tenacious
  Software LLC").
- **How it entered our tree**: Vendored amalgamation drop-in (`miniz.c`
  + `miniz.h`). Upstream commit hash unknown.

## Use case in this firmware

- Deflate / inflate / ZIP container support backing the EPUB unpacker
  in `libs/ra_epub/` (EPUB files are ZIP archives).
- Integrity claim category: data-handling (decompression of trusted
  local EPUB payloads).

## Qualification basis

Accepted as-is per IEC 61508-3 Section 7.4.2.12 and DO-178C Section
12.1.4:

- **Service history**: miniz has shipped as the single-file compression
  drop-in in countless game and tool projects since 2010.
- **Open-source community process**: Open GitHub project with public
  issue tracker.
- **Bug tracker review**: Issues at
  https://github.com/richgel999/miniz/issues reviewed; no open
  advisories at the 11.0.x release line affect read-only ZIP
  decompression of trusted local files.

## Risk mitigation

- miniz is exercised only on locally staged EPUB files; no network
  payload feeds it.
- The `libs/ra_epub/` wrapper enforces a maximum decompressed size cap
  to bound memory pressure.

## Deviations / patches

None. The vendored tree is unmodified.

## Last review date

- Reviewed: 2026-05-02
- Expected re-review by: 2027-05-02
