# ra8_ftl_demo

Demonstrates the Flash Translation Layer (`libs/ra8_ftl`) end to end over the
RA8D2's on-chip extra MRAM -- a non-volatile, erase-before-write medium
programmed through the MACI command sequencer. The FTL turns that into a
clean free-overwrite block device and spreads wear by relocating every
logical-block write to a fresh, least-worn physical block (copy-on-write).

The window is a couple of dozen 512-byte blocks. The demo declares two numbers,
the count of logical blocks to present and a reserved tail of one block, and
`ra8_ftl_mount()` does the rest: it reads the device's real block count, keeps
the tail back for the mapping checkpoint, and hands the FTL the blocks below it
so the remainder is relocation headroom. The reserved block is outside the FTL's
physical range by construction, and the checkpoint is written by `ra8_ftl_sync()`
rather than by the app driving raw erases and programs at an LBA it worked out
itself.

## The three acts

1. **Wear-levelling.** One logical block is overwritten repeatedly, and after
   each write `ra8_ftl_phys_of()` reports the physical block now backing it: the
   index migrates while the logical address stays fixed. Each write is read back
   and byte-verified, and the erase-count spread stays tight.
2. **Checkpoint.** One `ra8_ftl_sync()` call serialises the volatile mapping
   tables and programs them into the reserved tail. The app names no LBA and
   sizes no blob; the mount already knows where the checkpoint lives.
3. **Power-cycle survival.** The demo models a reset by zeroing the FTL handle
   and its caller tables -- SRAM is volatile -- while the MRAM retains its bytes.
   A naive `ra8_ftl_init()` has lost the mapping, and the logical block reads
   back the erase value, which is the proof; mounting the same medium again
   reports `k_ra8_ftl_mount_resumed` and restores both the data and the exact
   physical mapping. `ra8_ftl_unmount()` then ends the run in order.

   That naive re-init is kept deliberately as the counter-example. It is the
   entry point that takes a physical count and knows nothing about a
   checkpoint, which is what the mount lifecycle exists to replace.

## Why a checkpoint, and not automatic survival

`ra8_ftl` stores only user data in each physical block -- there is **no
per-block logical-address tag on the medium** -- so the logical-to-physical map
cannot be reconstructed from the data alone once the volatile tables are lost.
The minimal persistent metadata for survival is therefore an explicit, versioned
checkpoint of the mapping tables in a caller-chosen non-volatile block. The
checkpoint blob is architecture-local (native layout), so it is always restored
on the same device that wrote it.

## Blocked on

A bench run. Off-target the MACI program/erase sequence is modelled and the MRAM
retains its bytes for the whole run, so the in-process "power cycle" -- drop SRAM
state, keep MRAM -- is a faithful model of a real reset. Both the programming and
the retention-across-reset it depends on are therefore emulator-proven and
silicon-unverified.
