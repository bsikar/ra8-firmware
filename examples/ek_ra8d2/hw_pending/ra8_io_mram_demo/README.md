# ra8_io_mram_demo

Drives the `ra8_io` fabric over the RA8D2's on-chip extra MRAM (epic #155,
phase #156), programmed through the MACI command sequencer. Same block-device
vtable as the other `ra8_io` demos; only the backend differs
(`ra8_io_blockdev_mram` instead of RAM/SD/OSPI). It erases a 512-byte logical
block, programs a deterministic pattern, reads it back and byte-compares -- the
full erase + program + read path through the vtable -- and reports through a
`ra8_io` UART stream sink with `ra8_log` routed into the same stream, so a
failing step is visible (phase #157).

## Why raw block, not FAT

The window is **erase-before-write**: every write to a 512-byte sector must be
preceded by a full block erase, which clears it to `0xFF`. FAT requires free
overwrite of individual sectors, so bridging this medium to FAT needs a Flash
Translation Layer -- see `ra8_ftl_demo`. The window is in any case far too small
for a useful FAT12 volume once reserved sectors and FAT copies are accounted for.
So this demo drives the block-device layer directly and skips FAT/VFS entirely;
the FAT layer is already proven over the RAM, SD, OSPI and SDRAM backends, where
free overwrite is available.

## Blocked on

More than a bench run. As `src/main.c` warns, this window is **one-time-programmable
option-setting / OTP memory, not a rewritable data flash** -- there is no erase,
so the erase-and-reprogram cycle this demo exercises does not work on real
silicon. The emulator maps the window and the round-trip passes there, which is
optimistic rather than faithful. Closed issue #315 established that a virgin
GPOTP page can be programmed and read back on silicon, but it also confirmed
that the region has no erase operation. That result does not make this demo's
erase-and-reprogram cycle valid; the demo still needs a rewritable backend such
as OSPI or SD before hardware promotion.
