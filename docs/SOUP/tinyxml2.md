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
  `.h`). Upstream commit hash unknown.

## Use case in this firmware

- XML parser used by `libs/ra_epub/` to walk EPUB container metadata
  (`META-INF/container.xml`, OPF package documents, NCX navigation).
- Integrity claim category: data-handling (parsing of trusted local
  EPUB metadata).

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

- Parsed XML originates only from locally staged EPUB files; there is
  no network-driven XML path.
- All TinyXML-2 access is wrapped by `libs/ra_epub/` so the SOUP
  boundary is a single facade.

## Memory model on the firmware target

TinyXML-2 allocates its node pools (`MemPoolT<>::Alloc`), growable
arrays (`DynArray::EnsureCapacity`), and string storage
(`StrPair::SetStr`) through the global `operator new` / `operator new[]`.
The firmware is zero-heap (NASA Rule 3: `_sbrk` traps), so those operators
are replaced, firmware-only, by `libs/ra_epub/src/ra_epub_cpp_alloc.cpp`,
which routes them through the same bounded static first-fit arena miniz
uses (`ra_epub_miniz_alloc`). No TinyXML-2 allocation reaches `malloc`.
The host unit-test build keeps the standard `malloc`-backed operators
(the override is gated `#ifndef RA_SIMULATOR_MODE`).

**Known limitation -- fault, not error, on pool exhaustion.** The target
is built `-fno-exceptions`, so the replacement `operator new` returns
`nullptr` on exhaustion. TinyXML-2 does NOT null-check its `new[]`
results (e.g. `StrPair::SetStr`, `XMLDocument::Parse`), so an exhausted
pool faults in the following `memcpy` instead of surfacing
`XML_ERROR_*`. This is accepted rather than patched (the vendored
sources stay unmodified) because it cannot be reached by a conformant
book: the only documents parsed are `META-INF/container.xml` and the
OPF, and the OPF scratch is capped at `k_ra_epub_opf_xml_buf`, so the
node footprint is bounded well under the arena size. A future move to a
hand-rolled SAX scanner (tracked on the EPUB-reader roadmap) removes the
dependency entirely.

## Deviations / patches

The vendored `tinyxml2.cpp` / `tinyxml2.h` sources are unmodified. The
only integration seam is the external global `operator new` / `delete`
replacement described under "Memory model on the firmware target"; it
does not touch the SOUP component's own translation unit.

## Last review date

- Reviewed: 2026-06-19 (firmware memory-model integration added; SOUP
  basis re-confirmed)
- Expected re-review by: 2027-05-02
