# SOUP Justification: litehtml

Per IEC 61508-3 Section 7.4.2.12 and DO-178C Section 12.1.4, this document
records the qualification basis for accepting litehtml into this firmware
as Software Of Unknown Provenance (SOUP).

## Component identity

- **Name**: litehtml (HTML/CSS rendering engine)
- **Version**: 0.9+dev, pinned at upstream commit
  `8836bc1bc35ca0cfd71dc0386ef841d5cbc3bd5e` (default branch, 2026-01-10;
  340 commits past the `v0.9` release tag). The in-tree `CMakeLists.txt`
  sets `PROJECT_MAJOR=0`, `PROJECT_MINOR=0` because upstream only stamps
  a version at release time; the tree is a development-branch snapshot.
- **Upstream URL**: https://github.com/litehtml/litehtml
- **Local path**: `apps/shared_libs/third_party/litehtml/`

## Provenance

- **Origin**: Yuri Kobets (tordex), later community-maintained on
  GitHub.
- **License**: 3-clause BSD (`LICENSE`, "Copyright (c) 2013, Yuri
  Kobets (tordex). All rights reserved.").
- **How it entered our tree**: Vendored snapshot of the upstream default
  branch. The commit was recovered by fingerprinting: all 215 vendored
  files are byte-identical to upstream commit `8836bc1b`, the single
  exact match among the 1040 commits reachable from the upstream default
  branch. The vendored subset drops `doc/`, `support/`, `README.md` and
  the MSVC project files (20 files total) and nothing else.

## Use case in this firmware

- HTML/CSS layout engine for rendering EPUB content inside
  `apps/shared_libs/epub/`, used by the `apps/board/stand_alone/ereader` app.
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

- litehtml only renders untrusted local EPUB files staged on the file system; no
  network input feeds it.
- All access is through `apps/shared_libs/epub/`, which sandboxes the renderer
  to a fixed framebuffer in `libs/ra8_gfx/`.

## Deviations / patches

None. The vendored tree is unmodified (byte-identical to the pinned
upstream commit; documentation and MSVC scaffolding omitted).

## CVE monitoring

The pinned commit is queried against OSV.dev weekly by
`.github/workflows/osv-scan.yml` (commit-range GIT queries via
`scripts/checks/osv_scan.sh`); a published advisory affecting the pin
fails the scheduled run. Because litehtml parses untrusted EPUB
HTML/CSS, prefer re-vendoring at a tagged release when one lands.

## Last review date

- Reviewed: 2026-07-15 (commit pin recovered and recorded)
- Expected re-review by: 2027-05-02
