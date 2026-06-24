# ra_io_mram_demo -- ra_io fabric over on-chip extra MRAM (epic #155)

Proves the `ra_io` fabric over the RA8D2's on-chip **extra MRAM** (data flash) at
`0x27000000` -- a non-volatile, erase-before-write medium programmed through the
MACI command sequencer. Same VFS API as the other `ra_io` demos; only the
block-device backend differs (`ra_io_blockdev_mram`).

The extra-MRAM region is small (12 KiB), so the demo lays a tiny FAT12 volume
over the whole window and round-trips one small file.

What it exercises:

1. `ra_flash_init()` -- MRAM controller bring-up.
2. `ra_io_blockdev_mram_init()` -- bind an erase-before-write block device over
   the fenced window (Phase 1, #156).
3. Bridge to `ra_fs`, format/mount FAT12, register in the VFS as `mr`
   (Phase 3, #158).
4. Write `mr:/HELLO.TXT`, read it back through the VFS name, byte-compare.
5. Report on the SCI8 console through a `ra_io` UART stream sink; `ra_log` is
   routed into the same stream so any failing step is visible (Phase 2, #157).

board_sim models the MACI program/erase sequence (`board_periph_mram.c`), so the
round-trip runs headless.

## Expected output (board_sim or J-Link RTT/UART)

```
ra_io_mram_demo: boot
ra_io_mram_demo: mr:/HELLO.TXT 64 bytes PASS
```

## Build

```
make            # -> build/ra_io_mram_demo.elf
```

## Status

`hw_pending`: written and proven in board_sim; not yet bench-validated on a
physical EK-RA8D2.
