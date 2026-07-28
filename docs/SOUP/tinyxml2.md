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

## Use case in this firmware

- XML parser used by `libs/ra8_epub/` to walk EPUB container metadata
  (`META-INF/container.xml`, OPF package documents, NCX navigation).
- Also used by `libs/ra8_rabook_compile/` (the on-device EPUB->.rabook
  compiler) to parse each spine chapter's XHTML into the chapter DOM.
- Integrity claim category: data-handling (parsing of trusted local
  EPUB metadata and chapter content).

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
- All TinyXML-2 access is wrapped by `libs/ra8_epub/` so the SOUP
  boundary is a single facade.

## Memory model on the firmware target

TinyXML-2 allocates its node pools (`MemPoolT<>::Alloc`), growable
arrays (`DynArray::EnsureCapacity`), and string storage
(`StrPair::SetStr`) through the global `operator new` / `operator new[]`.
The firmware is zero-heap (NASA Rule 3: `_sbrk` traps), so those operators
are replaced, firmware-only, by `libs/ra8_epub/src/ra8_epub_cpp_alloc.cpp`,
which routes them through the same bounded static first-fit arena miniz
uses (`ra8_epub_miniz_alloc`). No TinyXML-2 allocation reaches `malloc`.
The host unit-test build keeps the standard `malloc`-backed operators
(the override is gated `#ifndef RA8_OFF_TARGET`).

**Known limitation -- fault, not error, on pool exhaustion.** The target
is built `-fno-exceptions`, so the replacement `operator new` returns
`nullptr` on exhaustion. TinyXML-2 does NOT null-check its `new[]`
results (e.g. `StrPair::SetStr`, `XMLDocument::Parse`), so an exhausted
pool faults in the following `memcpy` instead of surfacing
`XML_ERROR_*`. This is accepted rather than patched (the vendored
sources stay unmodified) because it cannot be reached by a conformant
book: the only documents parsed are `META-INF/container.xml` and the
OPF, and the OPF scratch is capped at `k_ra8_epub_opf_xml_buf`, so the
node footprint is bounded well under the arena size. A future move to a
hand-rolled SAX scanner (tracked on the EPUB-reader roadmap) removes the
dependency entirely.

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

Two integration seams exist:

1. **External `operator new` / `delete` replacement** (firmware only),
   described under "Memory model on the firmware target". It does not
   touch the SOUP component's own translation unit.

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

   - **Scope / blast radius.** Opt-in only. The patched branch is reached
     solely when a caller constructs the document with
     `XMLDocument(true, PEDANTIC_WHITESPACE)`; the only such caller is the
     chapter-content parse in
     `libs/ra8_rabook_compile/src/ra8_rabook_xml_shim.cpp`. `libs/ra8_epub/`
     and all metadata / OPF / container / TOC parsing keep the default
     `PRESERVE_WHITESPACE` mode, for which `WhitespaceMode() == PEDANTIC_WHITESPACE`
     is false and the patched guard never fires -- so the default parser
     behaviour is byte-for-byte unchanged. The change is covered by the
     real-book byte-identity gate `test_pipeline_parity_realbook_byte_identical`
     in `tests/test_ra8_rabook_pipeline.c`.

## Last review date

- Reviewed: 2026-06-28 (in-TU `Identify` PEDANTIC_WHITESPACE
  generalization added for the on-device compiler, #151; SOUP basis
  re-confirmed)
- Reviewed: 2026-06-19 (firmware memory-model integration added; SOUP
  basis re-confirmed)
- Expected re-review by: 2027-05-02
