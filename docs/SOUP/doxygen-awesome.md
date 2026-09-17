# SOUP Justification: Doxygen Awesome (documentation theme asset)

Per IEC 61508-3 Section 7.4.2.12 and DO-178C Section 12.1.4, this document
records the qualification basis for the vendored Doxygen Awesome stylesheet
and script set. It is a **documentation-site asset**, not firmware SOUP: none
of it is compiled, cross-compiled, or linked into any RA8 image, and no
firmware behaviour depends on it. It is recorded here because it is vendored
third-party source that this project redistributes (the generated
documentation site carries it verbatim), and because it was in the tree
uncatalogued for as long as it had been vendored (#629).

## Component identity

- **Name**: Doxygen Awesome (`doxygen-awesome-css`)
- **Version**: 2.4.2, tag `v2.4.2`, commit
  `d52eafe3e9303399fda15661f3d7bb8fe3d7eabc`
- **Upstream URL**: https://github.com/jothepro/doxygen-awesome-css
- **Local path**: `docs/doxygen_theme/`
- **Vendored subset**: 7 upstream files (`doxygen-awesome.css`,
  `sidebar-only.css`, `sidebar-only-darkmode-toggle.css`,
  `darkmode-toggle.js`, `fragment-copy-button.js`, `paragraph-link.js`,
  `LICENSE`), all byte-identical to the tag. Upstream carries 38 files at
  `v2.4.2`; the rest (its own docs, examples, and CI) are not vendored.

## Provenance

- **Origin**: jothepro (Bernhard Rosenkraenzer et al., upstream project
  `doxygen-awesome-css`).
- **License**: MIT. Text: `docs/doxygen_theme/LICENSE`
  ("Copyright (c) 2021 - 2023 jothepro").
- **How it is consumed**: referenced by `HTML_EXTRA_STYLESHEET` and
  `HTML_EXTRA_FILES` in the repository `Doxyfile`, and loaded by the
  generated HTML at view time. It never enters a compiler or linker.
- **Machine verification**: pinned in `scripts/gen/sbom_registry.py`, digested
  and published in `docs/sbom/ra8-firmware.cdx.json`, and re-verified file by
  file against the upstream tag by
  `scripts/checks/check_soup_upstream.py` from
  `docs/sbom/upstream/doxygen-awesome-css.manifest`.

## Use case in this project

- Styling for the generated Doxygen API documentation: the light/dark theme,
  the sidebar-only layout, the dark-mode toggle, the code-fragment copy
  button, and paragraph anchor links.

## Qualification basis

- **Outside the firmware trust boundary**: the asset is CSS and browser
  JavaScript served from the documentation site. A defect in it can only
  mis-render documentation; it cannot affect a firmware artifact, a build
  output, or a test verdict.
- **Not a build input**: unlike Vela (`docs/SOUP/vela.md`), which produces an
  artifact that is baked into firmware, this theme contributes nothing to any
  shipped binary.
- **Pinned and verified**: held at an exact upstream tag and commit, with
  every vendored byte re-derived against upstream on every CI run, so a
  silent in-place edit or an unnoticed version bump fails the gate.

## Deviations / patches

One local file, declared in the registry and in the upstream manifest:

- `docs/doxygen_theme/header.html` -- **first-party**, not an upstream file.
  It is this project's own Doxygen 1.16.1 header template carrying the theme's
  toggle hooks. It is kept in this directory because that is where the theme
  it hooks into lives; it is MIT under the root `LICENSE.txt`, like the rest of
  the first-party tree.

No upstream file is patched. The vendored subset is byte-identical to
`v2.4.2`.

## Redistribution note

`scope` in the registry is `excluded`: the theme is not part of any shipped
firmware image, so it does not belong in a firmware bill of materials'
included set. It **is** publicly redistributed with the generated
documentation site, which is what the MIT notice obligation attaches to, so
the `LICENSE` file is vendored alongside the assets and reproduced in
`THIRD_PARTY_LICENSES.md`.

## Last review date

- Reviewed: 2026-09-17
- Expected re-review by: 2027-09-17
