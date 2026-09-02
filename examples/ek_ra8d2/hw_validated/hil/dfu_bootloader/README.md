# dfu_bootloader

The immutable first 128 KiB of code-MRAM. It never erases itself. At every reset
it either boots a valid application slot -- software A/B select plus copy-to-run
-- or, when no slot is bootable or an application asked for it, brings up a
USB-DFU device and programs the inactive slot. Built on the controller-agnostic
`libs/ra8_dfu` core that the `dfu_selftest_*` HIL twins validate on both USB
controllers.

## Bank layout (1 MiB MRAM)

```
0x02000000  bootloader  128 KiB  (never erased by DFU)
0x02020000  Slot A      448 KiB  [image body | 32-byte header in the last page]
0x02090000  Slot B      448 KiB  [image body | 32-byte header in the last page]
0x02100000  (end)
```

The header is the slot's **last** 32-byte page, so the body starts at
`slot_base`, and it is programmed **last**. That ordering is the entire
brick-safety argument: a torn download leaves a slot whose CRC does not match,
never a half-valid one that boots. (With copy-to-run the body is relocated
before it executes, so header-last buys atomicity only -- not vector-table
alignment.)

## Boot decision

Pure, and MC/DC-tested in `tests/misc/src/test_ra8_dfu_boot.c`. Read and clear the
one-shot no-init SRAM trigger word; validate each slot (magic ok, length in
range, and a software CRC32 over the body equal to the stored CRC); then stay in
DFU if the trigger was set or neither slot is valid, otherwise copy-to-run the
valid slot with the higher sequence number, Slot A on a tie.

A freshly written bad slot fails its CRC, so the older valid slot still boots.
The bootloader region is never written by DFU, so an SWD re-flash always
recovers the board: there is no state a download can leave the device in that a
cable cannot undo.

On the DFU path the device comes up on USB-FS (J11), targets the inactive slot,
programs each download block straight into MRAM through the secure gate with an
SRAM-resident program loop, writes the header on end-of-download, and
soft-resets so the next boot decision picks the freshly written slot. The host
sends the body only; the bootloader writes the header.

## One image, either slot (copy-to-run, issue #97)

A slot is staging only. On a valid-slot boot the bootloader copies the body to a
single fixed SRAM run base and launches it there via the shared
`ra8_dfu_launch`, so a payload is **linked once** and the identical binary boots
from Slot A or Slot B -- no per-slot build, and no "which slot am I building
for?" footgun.

The obvious alternative, true position-independent code (ROPI/RWPI), does not
work here: this codebase's interface structs store absolute function pointers in
`.rodata`, and those cannot be relocated at load time.

The header records the run base as its entry, but the bootloader copies to the
**trusted constant**, not to the header field. A corrupted entry therefore only
fails the `ra8_dfu_run_target_valid` cross-check and drops the boot to DFU; it
can never redirect the copy.

`examples/ek_ra8d2/hw_validated/hil/dfu_bootloader/scripts/stage_slot_image.py`
builds a flashable slot image -- body plus header at the right offset -- from a
payload binary, for staging over J-Link with no USB host
involved. `dfu_copy_to_run` is the unattended proof that the copy-to-run
hand-off actually executes on silicon.
