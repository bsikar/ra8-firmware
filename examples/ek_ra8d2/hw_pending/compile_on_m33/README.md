<!--
Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
-->

# compile_on_m33

The complete bounded EPUB-to-RABOOK1 path running on the Cortex-M33 (#149b):
miniz reads the archive, `ra8_xml` and the EPUB consumers parse metadata and
chapters, and `libs/ra8_rabook_compile` emits the result. On a real import the
heavy conversion belongs on the M33 @ 250 MHz precisely so the M85 @ 1 GHz stays
free to keep the reader UI live. Here the M33 builds a small compiled book and
the M85 proves it is well-formed.

## What it teaches

- **Zero-allocation arenas (NASA Rule 3).** The emitter never `malloc`s; it only
  appends into caller-owned arenas. Its scratch tables live in the M33 image's
  own `SRAM_CPU1` `.bss`, and only the finished blob lands in the shared output
  buffer -- so the M85 reads the result with no copy.
- **Cross-core handoff via shared SRAM.** The mailbox sits at `0x22100000` and
  the output blob just above it, both inside the shared upper-SRAM window and
  below the M33's own bank at `0x22190000`. The M33 writes; the M85 reads only
  after `done`.
- **The M85 validates the result.** It runs `ra8_book_validate` over the shared
  blob (magic, version, table extents, body CRC-32) and cross-checks the
  M33-reported CRC and chapter count -- the same gate the real reader applies
  before walking a book.

## Current boundary

The M33 image proves unzip, bounded XML parsing, chapter compilation and
serialization. Raster transcoding is compiled out with `RA8_RABOOK_NO_RASTER`,
SVG payloads are stored verbatim, and the M85 still owns the filesystem and
staging policy.
