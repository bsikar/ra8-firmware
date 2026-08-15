# SOUP Justification: TinyXML-2

Per IEC 61508-3 Section 7.4.2.12 and DO-178C Section 12.1.4, this document
records the qualification basis for accepting TinyXML-2 into this firmware
as Software Of Unknown Provenance (SOUP).

## Component identity

- **Name**: TinyXML-2
- **Version**: 11.0.0 (per `tinyxml2.h`: TIXML2_MAJOR_VERSION = 11,
  TIXML2_MINOR_VERSION = 0, TIXML2_PATCH_VERSION = 0).
- **Upstream URL**: https://github.com/leethomason/tinyxml2
- **Local path**: `libs/third_party/tinyxml2/`
  - Files in tree: `tinyxml2.cpp`, `tinyxml2.h`, `LICENSE.txt`.

## Provenance

- **Origin**: Lee Thomason; community-maintained on GitHub.
- **License**: zlib license (`LICENSE.txt`, "This software is provided
  'as-is', without any express or implied warranty...").
- **How it entered our tree**: Vendored amalgamation drop-in (`.cpp` +
  `.h`). Resolved (#548) to release tag `11.0.0`, commit
  `9148bdf719e997d1f474be6bcc7943881046dba1`. `tinyxml2.h` and
  `LICENSE.txt` are byte-identical to it; `tinyxml2.cpp` differs only by
  the #151 patch recorded below.

## Current disposition

TinyXML-2 is retained only as a pinned historical SOUP snapshot. No current
first-party production or test target compiles or links it. EPUB metadata and
RABOOK chapter XHTML now use `libs/ra8_xml`, a caller-owned bounded pure-C pull
reader with explicit failure propagation.

## Qualification basis

Accepted as-is per IEC 61508-3 Section 7.4.2.12 and DO-178C Section
12.1.4:

- **Service history**: TinyXML-2 has shipped in many embedded and
  desktop projects since 2012.
- **Open-source community process**: Open GitHub project with public
  issue tracker and a maintained release line.
- **Bug tracker review**: Issues at
  https://github.com/leethomason/tinyxml2/issues reviewed; no open
  advisories at 11.0.0 affect read-only parsing of trusted local
  metadata.

## Risk mitigation

The component is outside the active binary and parser attack surface. Its
version, license, upstream bytes, and historical local patch remain pinned so
old release provenance stays reproducible.

## Retired memory model

The former integration routed global C++ allocation through an EPUB arena, but
could fault rather than propagate allocation failure. That integration and its
global operators were deleted when the bounded pull reader replaced TinyXML-2.

## Deviations / patches

The exact set is declared in `scripts/gen/sbom_registry.py` and pinned by
content in `docs/sbom/upstream/tinyxml2.manifest`, which the `soup-upstream`
gate re-checks on every CI run (#548): `tinyxml2.h` and `LICENSE.txt` are
byte-identical to tag `11.0.0`, and `tinyxml2.cpp` differs only by seam 2
below.

Neither source file was byte-identical before #548 -- the vendor-in sweep
(`75b635cc7`) ran the project formatter over both, so they differed from the
release throughout by preprocessor indentation and macro continuations. Both
were restored to upstream's bytes and the #151 patch re-applied on top, so the
recorded deviation is now exactly the deviation that exists.

Two historical integration seams existed:

1. **External `operator new` / `delete` replacement** (firmware only), now
   deleted with the former consumer.

2. **In-TU patch to `XMLDocument::Identify` (#151).** A single,
   behaviour-preserving generalization of the `PEDANTIC_WHITESPACE`
   branch in `tinyxml2.cpp`.

   - **What changed.** Upstream emits a whitespace text node only when
     the skipped whitespace immediately precedes a *closing* tag and the
     run is the *first* child being identified:
     `WhitespaceMode() == PEDANTIC_WHITESPACE && first && p != start && *(p + elementHeaderLen) == '/'`.
     The patch widens the guard to
     `WhitespaceMode() == PEDANTIC_WHITESPACE && p != start`, so any
     inter-element whitespace (before an opening *or* closing tag,
     anywhere in the document) becomes a text node. The branch body is
     unchanged (`CreateUnlinkedNode<XMLText>`, back the cursor up to the
     run start, restore the parse line).

   - **Why.** The on-device EPUB->.rabook compiler
     (`libs/ra8_rabook_compile`) must emit a `.rabook` blob byte-identical
     to the desktop reference `tools/epub_compile/epub_compile.py`. That
     reference uses Python's `HTMLParser`, which keeps every text run --
     including inter-element whitespace. With the default
     `PRESERVE_WHITESPACE` mode (and even upstream `PEDANTIC_WHITESPACE`)
     TinyXML-2 drops the whitespace run between two elements, so
     significant inline whitespace such as the space in
     `<abbr>x</abbr> <abbr>y</abbr>` is lost and the words merge into
     `xy`. The patch restores that whitespace for the compiler's
     chapter-DOM round-trip, giving byte-identity (and correct rendering)
     for on-device-compiled books.

   - **Scope / blast radius.** No active caller remains. The patch is retained
     solely to preserve the exact historical vendored bytes recorded by the
     upstream manifest.

## Last review date

- Reviewed: 2026-08-15 (production and test consumers removed; retained only
  as a pinned historical snapshot)
- Reviewed: 2026-06-28 (in-TU `Identify` PEDANTIC_WHITESPACE
  generalization added for the on-device compiler, #151; SOUP basis
  re-confirmed)
- Reviewed: 2026-06-19 (firmware memory-model integration added; SOUP
  basis re-confirmed)
- Expected re-review by: 2027-05-02
