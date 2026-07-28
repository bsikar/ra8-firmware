<!--
Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
-->

# import_reader -- on-import EPUB to .rabook compile + cache + read (#151)

The on-silicon proof of the self-contained-appliance flow from issue #151: drop
a raw `.epub` on the SD card and the device "just works." The first open
compiles the book **once** into the flat, execute-in-place `RABOOK1` `.rabook`
format and caches it on the card; every later open takes the fast cached path.

## What it does

1. Brings up a micro-SD over `ra8_sdmmc_spi` (Pmod2 / SCI0 Simple-SPI) and
   **mounts** the card's existing FAT volume with `ra8_fs` (it does **not**
   reformat -- the source `.epub` is preserved).
2. Drives the `ra8_rabook_import` cache manager wired to its **production** compile
   adapter `ra8_rabook_import_compile_adapter` (which streams the source through
   a bounded `ra8_vmem` page cache -- no whole-file load buffer, #230 -- into
   `ra8_rabook_compile_from_epub`):
   - **first open** of `BOOK.EPB` is a cache **miss** -> the importer keys the
     entry by the source CRC-32, compiles the EPUB to the bare `RABOOK1` body,
     and writes it crash-safely (temp + rename) as the root-level 8.3 cache name
     `XXXXXXXX.RBK` plus a freshness marker `XXXXXXXX.RBM` (outcome `compiled`);
   - the app confirms the `.rabook` now exists on the card;
   - **second open** of the same source is a cache **hit** -> the marker matches
     so the importer returns the cached path **without** recompiling (outcome
     `hit`, the compiler seam is never invoked).
3. Reads the cached `.rabook` back, `ra8_book_validate`s the bare (uncompressed)
   blob, and walks it -- logging the title, chapter count, and each chapter's
   plain-text length.

On success it prints exactly:

```
import_reader: miss->compile->cache->hit->read PASS
```

Any failed step prints `import_reader: FAIL <stage>` and parks the core.

## Running it

### ra8_emulator (headless)

Build a FAT16 card image carrying a text-only `.epub` as `BOOK.EPB` (the
generic single-file image builder works for any file):

```sh
tools/mkfontimg/build/mkfontimg my.epub card.img BOOK.EPB
ra8_emulator/build/ra8_emulator build/import_reader.elf --sd card.img
```

### On the bench

Insert a microSD carrying a `BOOK.EPB` text EPUB into a Digilent PMOD MicroSD
(part 410-380) in Pmod2 (J25), flash, and watch the J-Link OB console.

## Notes / next increment

- The cache uses the library's current **v1 root-level 8.3** name layout. The
  dedicated `/RABOOK/` subdirectory layout from the issue is **blocked** on FAT
  subdirectory write (`ra8_fs_mkdir`, tracked by #151/#165) and is the next
  increment.
- The compile working arenas live in external **SDRAM** (the conversion-arena
  tenant of #147) and are sized here for a small text book. An image-heavy book
  needs the larger ~24-32 MiB budget the issue specifies.
