<!-- Copyright (c) 2026 Brighton Sikarskie / SPDX-License-Identifier: MIT -->
# mpu_boot_map_hal

Brings the boot MPU memory-attribute map up through `ra8_mpu_apply_boot_map()`
instead of the hand-rolled MAIR / RBAR / RLAR / CTRL register pokes the shared
board boot uses (#576). It is the HAL-path twin of `cache_mpu_hil`: every
core-bring-up step is identical, and roughly ninety lines of raw region
programming collapse to a single call.

## Why a dedicated HAL entry point

The canonical boot map has five regions:

| # | Region | Range | Attr |
|---|--------|-------|------|
| 0 | MRAM code | `0x02000000` +1 MiB | RO + X, Normal WB/WA |
| 1 | M85-private SRAM0+1 | `0x22000000` +1 MiB | RW/XN, Normal WB/WA |
| 2 | SDRAM | `0x68000000` +64 MiB | RW/XN, Normal WB/WA |
| 3 | Peripherals | `0x40000000` +128 MiB | RW/XN, Device-nGnRE |
| 4 | Shared M85<->M33 SRAM | `0x22100000` +**640 KiB** | RW/XN, Normal non-cacheable |

Region 4 is **640 KiB -- not a power of two**, so the size-checked
`ra8_mpu_set_region()` rejects it outright. `ra8_mpu_apply_boot_map()` encodes
the map by base and limit, which the Armv8-M PMSAv8 RBAR/RLAR pair supports
natively. That is the missing primitive this app exercises.

## Self-test

Three independent checks gate the banner: `ra8_mpu_is_enabled()` confirms the
boot map was applied, without a direct register poke; `ra8_mpu_boot_map()`
exposes the region table and region 4 must be the 640 KiB non-cacheable shared
bank; and a live `SCKDIVCR` readback proves region 3 really is mapped
Device-nGnRE and reachable. Any failure prints a distinct line that never
contains `PASS`, and the core parks in WFI.
