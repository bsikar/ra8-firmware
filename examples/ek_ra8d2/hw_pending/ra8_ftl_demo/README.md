# ra8_ftl_demo -- wear-levelling + power-cycle survival over the FTL (#258)

Demonstrates the Flash Translation Layer (`ra8_ftl.h`) end to end over the
RA8D2's on-chip **extra MRAM** (data flash) at `0x27000000` -- a non-volatile,
erase-before-write medium. The FTL turns that erase-before-write device into a
clean free-overwrite block device and spreads wear by relocating every
logical-block write to a fresh, least-worn physical block (copy-on-write).

## Layout of the 12 KiB extra-MRAM window

The window is 24 logical blocks (512 bytes each). The demo hands the FTL the
first **23** physical blocks and presents **8** logical blocks (so 15 spare
blocks are available as relocation headroom), and reserves the **last** physical
block (index 23) as a non-volatile slot for the FTL mapping checkpoint. The FTL
never touches the reserved block; the demo erases and programs it directly
through the raw block device.

## What the demo exercises

1. **Wear-levelling.** `ra8_ftl_init()` over the raw MRAM device, then one
   logical block is overwritten 12 times. After each write, `ra8_ftl_phys_of()`
   reports the physical block now backing it -- the index migrates while the
   logical address stays fixed. Each write is read back and byte-verified, and
   `ra8_ftl_wear_stats()` prints the (tight) erase-count spread.
2. **Checkpoint.** `ra8_ftl_checkpoint_save()` serialises the volatile mapping
   tables (`map` + `pblocks`) into one 512-byte block, which is programmed into
   the reserved MRAM block. The FTL keeps no on-media metadata of its own, so
   this checkpoint is what lets the mapping outlive a reset.
3. **Power-cycle survival.** The demo models a reset: it zeroes the FTL handle
   and its caller tables (SRAM is volatile) while the MRAM retains its bytes. A
   naive `ra8_ftl_init()` has lost the mapping -- the logical block now reads
   back the erase value (`0xFF`), proving the point -- so
   `ra8_ftl_checkpoint_load()` reloads the checkpoint from MRAM and both the
   data and the exact physical mapping reappear.

## Why a checkpoint (and not automatic survival)

`ra8_ftl` stores only user data in each physical block -- there is no per-block
logical-address tag on the medium, so the logical->physical map cannot be
reconstructed from the data alone after the volatile tables are lost. The
minimal persistent metadata for survival is therefore an explicit, versioned
checkpoint of the mapping tables (`ra8_ftl_checkpoint_save`/`_load`), stored in a
caller-chosen non-volatile block. The checkpoint blob is architecture-local
(native layout) -- always restored on the same device that wrote it.

## ra8_emulator vs. silicon

ra8_emulator models the MACI program/erase sequence (`board_periph_mram.c`) and the
MRAM retains its bytes for the whole run, so the in-process "power cycle" (drop
SRAM state, keep MRAM) is a faithful model of a real reset and the demo runs
headless. Run it with:

```
tools/ra8_emulator/build/ra8_emulator build/ra8_ftl_demo.elf
```

## Expected output (ra8_emulator or J-Link RTT/UART)

```
ra8_ftl_demo: boot
ra8_ftl_demo: logical 2 now at physical <N>       (x12, index migrating)
ra8_ftl_demo: wear erase-count max <hi> min <lo>
ra8_ftl_demo: naive re-open lost the mapping (reads erase value)
ra8_ftl_demo: wear-level + power-cycle-survive on extra MRAM PASS
```

## Build

```
make            # -> build/ra8_ftl_demo.elf
```

## Status

`hw_pending`: written and proven in ra8_emulator; not yet bench-validated on a
physical EK-RA8D2. Like `ra8_io_mram_demo`, it programs on-chip MRAM, and it
additionally relies on MRAM retention across a reset -- both are ra8_emulator-proven
but silicon-unverified here.
