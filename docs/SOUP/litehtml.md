# SOUP Justification: litehtml

Per IEC 61508-3 Section 7.4.2.12 and DO-178C Section 12.1.4, this document
records the qualification basis for accepting litehtml into this firmware
as Software Of Unknown Provenance (SOUP).

## Component identity

- **Name**: litehtml (HTML/CSS rendering engine)
- **Version**: unknown (the in-tree `CMakeLists.txt` sets
  `PROJECT_MAJOR=0`, `PROJECT_MINOR=0` and there is no separate VERSION
  file; this corresponds to an upstream development-branch snapshot).
- **Upstream URL**: https://github.com/litehtml/litehtml
- **Local path**: `libs/third_party/litehtml/`

## Provenance

- **Origin**: Yuri Kobets (tordex), later community-maintained on
  GitHub.
- **License**: 3-clause BSD (`LICENSE`, "Copyright (c) 2013, Yuri
  Kobets (tordex). All rights reserved.").
- **How it entered our tree**: Vendored snapshot of the upstream
  litehtml repository. Upstream commit hash unknown.

## Use case in this firmware

- HTML/CSS layout engine for rendering EPUB content inside
  `libs/ra_epub/`, used by the `examples/ek_ra8d2/ereader` demo.
- Integrity claim category: none (display-only EPUB rendering).

## Qualification basis

Accepted as-is per IEC 61508-3 Section 7.4.2.12 and DO-178C Section
12.1.4:

- **Service history**: litehtml has been used as the embedded HTML
  rendering core in CHM viewers and several offline-help tools since
  2013.
- **Open-source community process**: Open GitHub project with public
  issue tracker.
- **Bug tracker review**: Issues at
  https://github.com/litehtml/litehtml/issues reviewed; no open
  advisories affect read-only rendering of trusted local EPUB content.

## Risk mitigation

- litehtml only renders local EPUB files staged on the file system; no
  network input feeds it.
- All access is through `libs/ra_epub/`, which sandboxes the renderer
  to a fixed framebuffer in `libs/ra_gfx/`.

## Deviations / patches

None. The vendored tree is unmodified.

## Last review date

- Reviewed: 2026-05-02
- Expected re-review by: 2027-05-02
