# ra8_io_mram_demo -- ra8_io fabric over on-chip extra MRAM (epic #155)

Proves the `ra8_io` fabric over the RA8D2's on-chip **extra MRAM** (data flash)
at `0x27000000` -- a non-volatile, erase-before-write medium programmed through
the MACI command sequencer. Same block-device vtable as the other `ra8_io` demos;
only the backend differs (`ra8_io_blockdev_mram` instead of RAM/SD/OSPI).

## Why raw block, not FAT

The extra-MRAM window is exactly **12 KiB** and is **erase-before-write**: every
write to a 512-byte sector must be preceded by a full block erase, which clears
the sector to 0xFF. FAT requires free overwrite of individual sectors (write a
new cluster-chain entry without erasing the whole block). Bridging erase-before-
write storage to FAT requires a **Flash Translation Layer (FTL)** that remaps
logical sectors to physical erase blocks and performs wear-levelling. No such FTL
exists in this tree yet (see follow-up issue). The 12 KiB total capacity is also
too small for a useful FAT12 volume once reserved sectors and FAT copies are
accounted for.

This demo therefore drives the block-device layer directly and skips the FAT/VFS
path. The FAT layer is already proven over the RAM, SD, OSPI, and SDRAM backends
where free overwrite is available.

## What the demo exercises

1. `ra8_flash_init()` -- MRAM controller bring-up (Phase 1, #156).
2. `ra8_io_blockdev_mram_init()` -- binds an erase-before-write block device over
   the fenced 12 KiB window.
3. Erase a 512-byte logical block (fills to 0xFF via MACI), program a
   deterministic byte pattern, read it back, and byte-compare -- the full
   erase + program + read path through the block-device vtable.
4. Report on the SCI8 console through a `ra8_io` UART stream sink; `ra8_log` is
   routed into the same stream so any failing step is visible (Phase 2, #157).

ra8_emulator models the MACI program/erase sequence (`board_periph_mram.c`), so the
round-trip runs headless.

## Expected output (ra8_emulator or J-Link RTT/UART)

```
ra8_io_mram_demo: boot
ra8_io_mram_demo: 512-byte block erase/program/read on extra MRAM PASS
```

## Build

```
make            # -> build/ra8_io_mram_demo.elf
```

## Status

`hw_pending`: written and proven in ra8_emulator; not yet bench-validated on a
physical EK-RA8D2.
