# dfu_bootloader -- immutable USB-DFU MRAM bootloader

The first **128 KiB** of code-MRAM. It never erases itself: at every reset it
either **boots the valid application slot** (software A/B select + jump) or, when
no slot is bootable (or an app requested it), brings up a **USB-DFU device** and
programs the inactive slot. Built on the controller-agnostic
[`libs/ra_dfu`](../../../../../libs/ra_dfu) core that the bidirectional HIL twins
([`dfu_selftest_hs_host`](../../hw_validated/hil/dfu_selftest_hs_host) /
[`dfu_selftest_fs_host`](../../hw_validated/hil/dfu_selftest_fs_host)) validate
on both USB controllers.

## Bank layout (1 MiB MRAM)

```
0x02000000  bootloader  128 KiB  (immutable; never erased by DFU)
0x02020000  Slot A      448 KiB  [app image | 32-byte header in the last page]
0x02090000  Slot B      448 KiB  [app image | 32-byte header in the last page]
0x02100000  (end)
```

The image **header is the slot's last 32-byte page** (`slot_base + 0x6FFE0`), not
its first, so the application's vector table lands at the 64 KiB-aligned
`slot_base` -- a valid `VTOR.TBLOFF`. The header is
`magic | seq | img_len | img_crc32 | entry(=slot_base) | reserved`, programmed
**last** so a torn download leaves an invalid (CRC-mismatching) slot, never a
half-valid one.

## Boot decision (pure, MC/DC-tested in `tests/test_ra_dfu_boot.c`)

1. Read + clear the one-shot no-init SRAM trigger word `g_dfu_trigger`.
2. Validate both slots: `magic` ok **and** `img_len` in range **and** software
   CRC32 over `[slot_base, slot_base + img_len)` equals `img_crc32`.
3. `ra_dfu_boot_decide`: trigger set **or** neither slot valid -> stay in DFU;
   otherwise jump to the valid slot with the higher `seq` (Slot A on a tie).

A freshly written bad slot fails its CRC, so the older valid slot still boots --
brick-safe by construction. The bootloader region is never written by DFU, so
SWD re-flash is always available.

## DFU update flow

On the DFU path the bootloader brings up the USBX DFU device on **USB-FS (J11)**,
targets the **inactive** slot, programs each `DFU_DNLOAD` block straight into MRAM
(secure gate, SRAM-resident program loop), writes the header on end-of-download,
and soft-resets -- so the next boot decision selects the freshly written slot.

```
dfu-util -a 0 -D <app>_slotX.bin     # body only; the bootloader writes the header
```

## Per-slot payload builds

Because an in-place A/B image runs from its slot's absolute address, an
application must be **linked at its slot base** (no position independence yet --
that is tracked as PIC follow-up
[issue #97](https://github.com/bsikar/ra8d2-firmware/issues/97), which would let a
single `.bin` flash to either slot and remove this per-slot step).

```
Slot A base = 0x02020000   (app vector table at 0x02020000, header at 0x0208FFE0)
Slot B base = 0x02090000   (app vector table at 0x02090000, header at 0x020FFFE0)
```

Build the app for a slot by pointing its linker `MRAM` region at the slot base,
then objcopy to a raw `.bin`:

```
# Slot A: in the payload app's linker_script.ld set
#   MRAM (rx) : ORIGIN = 0x02020000, LENGTH = 448K
# Slot B: ORIGIN = 0x02090000, LENGTH = 448K
make <app>
arm-none-eabi-objcopy -O binary <app>.elf <app>_slotA.bin
```

`LENGTH` must be `448K` (not the default 1 MiB) so the link-time MRAM-overflow
ASSERT fires if the image would run past the slot. Then either let the
bootloader's DFU device program it (the operator path) or stage it directly over
J-Link (the no-host path):

```
# Operator path -- the bootloader writes the header on DFU commit:
dfu-util -a 0 -D <app>_slotA.bin

# No-host path -- wrap the body with its header and flash the slot directly:
python3 stage_slot_image.py --payload <app>_slotA.bin --slot a --seq 1 --out slotA.hex
JLinkExe ... loadfile slotA.hex      # body @slot_base + header @slot_base+0x6FFE0
```

> The per-slot link base is the manual step that PIC follow-up
> [issue #97](https://github.com/bsikar/ra8d2-firmware/issues/97) removes; until
> then each slot needs its own `ORIGIN`.

## Validation (on-bench, EK-RA8D2, 2026-06-16)

The DFU **program/read-back** path is validated in both directions by the HIL
twins (Config A HS->FS, Config B FS->HS). The bootloader's own boot decision and
jump were validated locally over the J-Link OB:

- **No bootable slot -> DFU** (both slots erased). SCI8:
  ```
  ra8d2 dfu-bootloader (immutable, 128K @ 0x02000000)
  dfu-bootloader: no bootable slot -- entering DFU
  dfu-bootloader: DFU device up on USB-FS, awaiting DFU_DNLOAD...
  ```
- **Valid slot -> jump**. A 64-byte Slot-A image (vector table + a reset stub
  that writes `0x600D600D` to `0x22040000`) was staged with `stage_slot_image.py`
  and flashed. After reset:
  ```
  dfu-bootloader: jumping to Slot A @0x02020000
  ```
  J-Link probes confirmed the hand-off executed: `mem32 0x22040000 = 600D600D`
  (the slot's reset stub ran) and `mem32 0xE000ED08 (VTOR) = 0x02020000`.

- **Full DFU flash -> boot cycle**, end to end, with no external `dfu-util`:
  the board is its own DFU host over the self-loop
  ([`dfu_selftest_boot`](../../hw_validated/hil/dfu_selftest_boot)). The HS host
  DFU_DNLOADs the same bootable Slot-A payload and sends a manifest; the FS DFU
  device programs **and commits** the slot header (`USB SELFTEST DFU-BOOT COMMIT
  PASS`). Flashing this bootloader afterward (Slot A untouched) and resetting
  then boots the DFU-committed image -- `jumping to Slot A` + `mem32 0x22040000
  = 600D600D`. So every code path the real `dfu-util` exercises (program,
  commit, boot decision, jump) is validated on bench; only the literal external
  `dfu-util` tool is unused, because the bench self-loops the two jacks.

## Recovery

The bootloader region is never erased by DFU, so a normal SWD re-flash of this
app (`scripts/flash.sh` / `make flash`) always recovers the board. A bad slot is
self-healing (CRC fails -> the older valid slot boots).
```
