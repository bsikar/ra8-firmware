# SOUP Justification: Eclipse GUIX

Per IEC 61508-3 Section 7.4.2.12 and DO-178C Section 12.1.4, this document
records the qualification basis for accepting Eclipse GUIX into this
firmware as Software Of Unknown Provenance (SOUP).

## Component identity

- **Name**: Eclipse GUIX (formerly Azure RTOS GUIX)
- **Version**: 6.5.0 (per `common/inc/gx_api.h` GUIX_MAJOR / MINOR /
  PATCH macros).
- **Upstream URL**: https://github.com/eclipse-threadx/guix
- **Local path**: `libs/third_party/guix/`

## Provenance

- **Origin**: Eclipse Foundation, Eclipse ThreadX top-level project
  (donated by Microsoft from Azure RTOS in 2024).
- **License**: MIT (`LICENSE.txt`, "Copyright (c) 2024 - present Microsoft
  Corporation").
- **How it entered our tree**: Vendored snapshot of the upstream Eclipse
  GUIX repository, including bundled `fonts/` and `graphics/` asset
  trees. Upstream commit hash unknown.

## Use case in this firmware

- Embedded GUI framework rendered on the EK-RA8D2 7-inch parallel TFT
  by `examples/ek_ra8d2/threadx_guix_demo`. Layered on top of
  `libs/ra_gfx/` for the pixel back end.
- Integrity claim category: none (operator-display only; no safety
  signal is read back from the GUI).

## Qualification basis

Accepted as-is per IEC 61508-3 Section 7.4.2.12 and DO-178C Section
12.1.4:

- **Service history**: Express Logic GUIX has shipped in embedded HMI
  panels since the mid-2010s.
- **Open-source community process**: Eclipse Foundation governance.
- **Bug tracker review**: Issues at
  https://github.com/eclipse-threadx/guix/issues reviewed; no open
  advisories affect the demo usage.

## Risk mitigation

- GUIX runs only inside `threadx_guix_demo`; not linked into any
  production-track app.
- All display-controller register access is mediated by `libs/ra_gfx/`.

## Deviations / patches

None. The vendored tree is unmodified.

## Last review date

- Reviewed: 2026-05-02
- Expected re-review by: 2027-05-02
