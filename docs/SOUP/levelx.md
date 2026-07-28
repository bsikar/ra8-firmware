# SOUP Justification: Eclipse LevelX

Per IEC 61508-3 Section 7.4.2.12 and DO-178C Section 12.1.4, this document
records the qualification basis for accepting Eclipse LevelX into this
firmware as Software Of Unknown Provenance (SOUP).

## Component identity

- **Name**: Eclipse LevelX (formerly Azure RTOS LevelX)
- **Version**: 6.5.0 (per `common/inc/lx_api.h` LEVELX_MAJOR / MINOR /
  PATCH macros).
- **Upstream URL**: https://github.com/eclipse-threadx/levelx
- **Local path**: `libs/third_party/levelx/`

## Provenance

- **Origin**: Eclipse Foundation, Eclipse ThreadX top-level project
  (donated by Microsoft from Azure RTOS in 2024).
- **License**: MIT (`LICENSE.txt`, "Copyright (c) 2024 - present Microsoft
  Corporation").
- **How it entered our tree**: Vendored snapshot of the upstream Eclipse
  LevelX repository. Resolved (#548) to release tag
  `v6.5.0.202601_rel`, commit `a46b74fb8aa133796ccbc13e7902cb8bb818e12f`:
  89 of the 90 vendored files are byte-identical to it, the exception
  being the `.gitattributes` edit recorded under "Deviations / patches".

## Use case in this firmware

- NOR-flash wear-leveling layer sitting between FileX and the Octo-SPI
  flash device. Used by `examples/ek_ra8d2/threadx_levelx_demo` and
  `threadx_filex_levelx_demo`.
- Integrity claim category: data-handling (logical-to-physical sector
  mapping, wear-level metadata).

## Qualification basis

Accepted as-is per IEC 61508-3 Section 7.4.2.12 and DO-178C Section
12.1.4:

- **Service history**: Express Logic LevelX has shipped in industrial
  flash-storage stacks since the mid-2010s.
- **Open-source community process**: Eclipse Foundation governance.
- **Bug tracker review**: Issues at
  https://github.com/eclipse-threadx/levelx/issues reviewed; no open
  advisories affect the demo usage.

## Risk mitigation

- Underlying flash driver shim lives in `libs/ra8_fs/`, so LevelX sees a
  single block-device interface.
- Demo-only use in this revision.

## Deviations / patches

One file, `.gitattributes`, and it is a repository-hygiene edit rather than a
change to any shipped source. Commit `368072a1a` dropped its two `[attr]`
attribute-macro blocks (`our-c-style`, `generated`) from all five vendored
Eclipse ThreadX trees: git honours `[attr]` definitions only in the top-level
`.gitattributes` and printed a "not allowed" warning for each on EVERY git
operation. The macro *uses* left behind reference undefined attributes, which
git ignores silently, so no vendored file's checkout behaviour changes.

Declared in `scripts/gen/sbom_registry.py` as `patched_files` and pinned by
content in `docs/sbom/upstream/levelx.manifest`; every other file in this
component is verified byte-identical to the upstream pin on each CI run
(#548).

The edit is from 2026-07-13 and went unrecorded here until #548 found it two
weeks later, which is the point: "the vendored tree is unmodified" was prose,
and prose does not notice a tree-wide sweep reaching into `libs/third_party/`.

## Last review date

- Reviewed: 2026-05-02
- Expected re-review by: 2027-05-02
