# ra8_ftl_demo

Demonstrates the Flash Translation Layer (`libs/ra8_ftl`) end to end over the
RA8D2's on-chip extra MRAM -- a non-volatile, erase-before-write medium
programmed through the MACI command sequencer (#258). The FTL turns that into a
clean free-overwrite block device and spreads wear by relocating every
logical-block write to a fresh, least-worn physical block (copy-on-write).

The window is a couple of dozen 512-byte blocks. The demo hands the FTL all but
the last, presents a smaller set of logical blocks so the remainder is relocation
headroom, and reserves that last physical block as a non-volatile slot for the
mapping checkpoint. The FTL never touches the reserved block; the demo erases and
programs it directly through the raw block device.

## The three acts

1. **Wear-levelling.** One logical block is overwritten repeatedly, and after
   each write `ra8_ftl_phys_of()` reports the physical block now backing it: the
   index migrates while the logical address stays fixed. Each write is read back
   and byte-verified, and the erase-count spread stays tight.
2. **Checkpoint.** `ra8_ftl_checkpoint_save()` serialises the volatile mapping
   tables into one block, which is programmed into the reserved MRAM block.
3. **Power-cycle survival.** The demo models a reset by zeroing the FTL handle
   and its caller tables -- SRAM is volatile -- while the MRAM retains its bytes.
   A naive re-init has lost the mapping, and the logical block reads back the
   erase value, which is the proof; `ra8_ftl_checkpoint_load()` then restores
   both the data and the exact physical mapping.

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
