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

- NOR-flash wear-levelling layer over the on-board Octo-SPI flash. It is
  built in two mutually exclusive modes and both are consumed, so "under
  FileX" describes only half of it:
  - **ThreadX-coupled** (`cmake/levelx.cmake`). `threadx_levelx_demo` drives
    the LevelX NOR API directly with no filesystem above it, while
    `threadx_filex_demo` and `threadx_filex_levelx_demo` mount a FAT volume on
    LevelX through `fx_media_driver_ra8_levelx`
    (`port/levelx/src/lx_filex_adapter.c`). All three live under
    `examples/ek_ra8d2/hw_validated/hil/`.
  - **Standalone** (`cmake/levelx_standalone.cmake`, built with
    `LX_STANDALONE_ENABLE`): no ThreadX and no FileX in the graph at all. The
    first-party `libs/ra8_cache_store/` (#201) is built on this mode, and
    `examples/ek_ra8d2/hil_needs_revalidation/ra8_cache_store_demo` exercises
    it against a RAM-backed NOR driver.
- The board-facing driver shim is `port/levelx/src/lx_nor_driver_ra8_xspi.c`.
  Host unit tests compile the vendored NOR sources directly
  (`tests/cmake/tests_storage.cmake`), so the component is exercised off-target
  as well.
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

- The underlying flash driver shim is `port/levelx/src/lx_nor_driver_ra8_xspi.c`
  (with `port/levelx/src/lx_filex_adapter.c` bridging FileX above it where a
  filesystem is used), so LevelX sees a single block-device interface and the
  whole hardware-facing surface is first-party code held to the full project
  bar. It is **not** in `libs/ra8_fs/`, which contains no LevelX code.
- No longer demo-only: `libs/ra8_cache_store/` is a production-intent
  first-party library sitting on the standalone build. The demos remain the
  hardware-verification vehicle -- `threadx_levelx_demo` (`[lx] sector rw
  verified readback=1`) and `threadx_filex_levelx_demo` were both verified
  live on 2026-06-10 per their `hil.conf` records -- and no safety-critical
  data is committed to NOR flash.

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
- Use case + risk mitigation re-verified against the tree and corrected
  (#616): 2026-08-04. The driver shim was recorded in `libs/ra8_fs/`, which
  holds no LevelX code, and the standalone / `ra8_cache_store` path was
  missing entirely.
- Expected re-review by: 2027-05-02
