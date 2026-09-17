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

- HTML/CSS layout engine behind the **v2** reflow engine
  (`apps/shared_libs/reflow/v2/src/reflow_v2.cpp`), a drop-in replacement
  for the hand-rolled v1 sources behind the same public API in
  `apps/shared_libs/reflow/inc/reflow.h`, which rasterises pages into a
  caller-owned framebuffer in `libs/ra8_gfx/`.
- v2 is compiled only when `REFLOW_USE_LITEHTML=ON`. That option is
  declared `OFF` in `apps/shared_libs/reflow/CMakeLists.txt` and again in
  `tests/cmake/library_sources.cmake`, so a default firmware or host-test
  configure builds the v1 engine and no litehtml translation unit at all.
  `docs/EPUB_CONFORMANCE.md` section 3 records that bench as a deliberate
  decision (C++/STL plus `malloc`, against NASA P10 Rule 3).
- With the option ON, the `litehtml` and `gumbo` targets are also linked
  into host test binaries (`tests/cmake/tests_xml.cmake`,
  `tests_npu.cmake`, `tests_crypto.cmake`, `core_hal.cmake` and
  `unit_tests.cmake`), which is where `test_reflow_v2` runs.
- `apps/shared_libs/epub/` does not reach litehtml: its container and
  markup path is miniz for the ZIP layer plus the bounded first-party XML
  pull reader in `apps/shared_libs/xml/`.
- `apps/board/stand_alone/ereader` does not build it either. That app's
  two-project TrustZone `CMakeLists.txt` names `ra8_tz_secure_boot` and
  `threadx_ns`, and no reflow or litehtml source.
- Integrity claim category: none (display-only EPUB rendering).

## Nested component: gumbo (Apache-2.0)

litehtml carries its own copy of the gumbo HTML5 parser at
`apps/shared_libs/third_party/litehtml/src/gumbo/`, under a separate
`LICENSE` (the canonical Apache-2.0 text, blob `d645695`) rather than
litehtml's 3-clause BSD. It sits inside the vendored path this record
pins, so the same commit pin and the same byte-for-byte upstream
verification cover it, and the `gumbo` CMake target is linked wherever
litehtml is, so it is never compiled on a default configure.

Google's upstream `gumbo-parser` is archived, so fixes reach this tree
only by re-vendoring litehtml, which is why the lineage is worth
watching. As filed in #618, gumbo has no SBOM component and no
licence-inventory row of its own today; it rides this record until that
follow-up lands.

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

- On a default configure the option above is OFF, so no litehtml or gumbo
  code is compiled into any firmware image or host test binary.
- Where it is enabled, litehtml renders untrusted local EPUB files staged
  on the file system; no network input feeds it.
- All access is through the v2 adapter in
  `apps/shared_libs/reflow/v2/`, which sandboxes the renderer to a fixed
  caller-owned framebuffer in `libs/ra8_gfx/`.

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
- Integration point re-verified: 2026-09-17 (against `dev` `8e70a3d`)
- Expected re-review by: 2027-05-02
