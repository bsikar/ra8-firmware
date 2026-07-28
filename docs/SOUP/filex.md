# SOUP Justification: Eclipse FileX

Per IEC 61508-3 Section 7.4.2.12 and DO-178C Section 12.1.4, this document
records the qualification basis for accepting Eclipse FileX into this
firmware as Software Of Unknown Provenance (SOUP).

## Component identity

- **Name**: Eclipse FileX (formerly Azure RTOS FileX)
- **Version**: 6.5.0 (per `common/inc/fx_api.h` FILEX_MAJOR / MINOR /
  PATCH macros).
- **Upstream URL**: https://github.com/eclipse-threadx/filex
- **Local path**: `libs/third_party/filex/`

## Provenance

- **Origin**: Eclipse Foundation, Eclipse ThreadX top-level project
  (donated by Microsoft from Azure RTOS in 2024).
- **License**: MIT (`LICENSE.txt`, "Copyright (c) 2024 - present Microsoft
  Corporation").
- **How it entered our tree**: Vendored snapshot of the upstream Eclipse
  FileX repository. Resolved (#548) to release tag
  `v6.5.0.202601_rel`, commit `bb6e295af079f3cd903272982106b0ddd9537422`:
  266 of the 267 vendored files are byte-identical to it, the exception
  being the `.gitattributes` edit recorded under "Deviations / patches".

## Use case in this firmware

- FAT12/16/32 / exFAT file system used by `examples/ek_ra8d2/threadx_filex_demo`
  and `threadx_filex_levelx_demo`. Layered above LevelX for raw-flash
  storage and above MMC/SD media drivers in `libs/ra8_fs/` for block
  devices.
- Integrity claim category: data-handling (filesystem metadata and
  user-file payloads).

## Qualification basis

Accepted as-is per IEC 61508-3 Section 7.4.2.12 and DO-178C Section
12.1.4:

- **Service history**: Express Logic FileX has shipped in millions of
  industrial and consumer products since the early 2000s.
- **Open-source community process**: Eclipse Foundation governance,
  per-release `SECURITY.md` policy.
- **Bug tracker review**: Issues at
  https://github.com/eclipse-threadx/filex/issues reviewed; no open
  advisories at vendor-in date affect our usage profile.
- **Vendor qualification data**: Pre-Eclipse, FileX carried SGS-TUV
  Saar pre-certifications for IEC 61508, IEC 62304, ISO 26262, and EN
  50128; cited for context only.

## Risk mitigation

- Block-device access is mediated through `libs/ra8_fs/` so the SOUP
  boundary is well defined.
- No safety-critical configuration is currently committed to the file
  system; FileX is used for log capture and OTA staging only.

## Deviations / patches

One file, `.gitattributes`, and it is a repository-hygiene edit rather than a
change to any shipped source. Commit `368072a1a` dropped its two `[attr]`
attribute-macro blocks (`our-c-style`, `generated`) from all five vendored
Eclipse ThreadX trees: git honours `[attr]` definitions only in the top-level
`.gitattributes` and printed a "not allowed" warning for each on EVERY git
operation. The macro *uses* left behind reference undefined attributes, which
git ignores silently, so no vendored file's checkout behaviour changes.

Declared in `scripts/gen/sbom_registry.py` as `patched_files` and pinned by
content in `docs/sbom/upstream/filex.manifest`; every other file in this
component is verified byte-identical to the upstream pin on each CI run
(#548).

The edit is from 2026-07-13 and went unrecorded here until #548 found it two
weeks later, which is the point: "the vendored tree is unmodified" was prose,
and prose does not notice a tree-wide sweep reaching into `libs/third_party/`.

## Last review date

- Reviewed: 2026-05-02
- Expected re-review by: 2027-05-02
