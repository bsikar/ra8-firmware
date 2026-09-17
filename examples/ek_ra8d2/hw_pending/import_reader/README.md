<!--
Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
-->

# import_reader

The self-contained-appliance flow from #151: drop a raw `.epub` on the SD card
and the device just works. The first open compiles the book **once** into the
flat, execute-in-place `RABOOK1` format and caches it on the card; every later
open takes the cached path.

The card's existing FAT volume is **mounted, never reformatted**, so the source
`.epub` survives. The importer keys its cache entry by the source CRC-32,
compiles through the production adapter -- which streams the source through a
bounded `ra8_vmem` page cache rather than a whole-file load buffer (#230) -- and
writes the result crash-safely (temp plus rename) alongside a freshness marker. A
second open of the same source matches the marker and returns the cached path
**without invoking the compiler seam at all**. The app then reads the cached
`.rabook` back, validates the bare blob, and walks it.

Reaching it needs a microSD carrying the source book, in a Pmod2 microSD adapter
over `ra8_sdmmc_spi` (SCI0 Simple-SPI).

## Notes

- The cache uses the library's current **root-level 8.3** name layout.
  `ra8_fs_mkdir` has since landed, so a dedicated `/RABOOK/` subdirectory is no
  longer blocked on FAT subdirectory writes; adopting that optional layout is
  simply outside this example's current scope.
- The compile working arenas live in external SDRAM -- the conversion-arena
  tenant of #147 -- and are sized here for a small text book. An image-heavy book
  needs the much larger budget the issue specifies.
