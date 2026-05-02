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

## Deviations / patches

None. The vendored sources are unmodified.

## Last review date

- Reviewed: 2026-05-02
- Expected re-review by: 2027-05-02
