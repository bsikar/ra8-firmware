# dfu_bootloader -- immutable USB-DFU MRAM bootloader

The first **128 KiB** of code-MRAM. It never erases itself: at every reset it
either **boots the valid application slot** (software A/B select +
**copy-to-run**: the slot body is copied to a fixed SRAM run base and launched
there, so one image boots from either slot -- see below) or, when no slot is
bootable (or an app requested it), brings up a **USB-DFU device** and programs
the inactive slot. Built on the controller-agnostic
[`libs/ra8_dfu`](../../../../../libs/ra8_dfu) core that the bidirectional HIL twins
([`dfu_selftest_hs_host`](../dfu_selftest_hs_host) /
[`dfu_selftest_fs_host`](../dfu_selftest_fs_host)) validate
on both USB controllers.

## Bank layout (1 MiB MRAM)

```
0x02000000  bootloader  128 KiB  (immutable; never erased by DFU)
0x02020000  Slot A      448 KiB  [app image | 32-byte header in the last page]
0x02090000  Slot B      448 KiB  [app image | 32-byte header in the last page]
0x02100000  (end)
```

The image **header is the slot's last 32-byte page** (`slot_base + 0x6FFE0`), so
the image body starts at `slot_base`. The header is
`magic | seq | img_len | img_crc32 | entry(=k_ra8_dfu_run_base) | reserved`,
programmed **last** so a torn download leaves an invalid (CRC-mismatching) slot,
never a half-valid one. (With copy-to-run the body is copied to the SRAM run base
before execution, so header-last is kept purely for that torn-write atomicity --
not for vector-table alignment.)

## Boot decision (pure, MC/DC-tested in `tests/test_ra8_dfu_boot.c`)

1. Read + clear the one-shot no-init SRAM trigger word `g_dfu_trigger`.
2. Validate both slots: `magic` ok **and** `img_len` in range **and** software
   CRC32 over `[slot_base, slot_base + img_len)` equals `img_crc32`.
3. `ra8_dfu_boot_decide`: trigger set **or** neither slot valid -> stay in DFU;
   otherwise copy-to-run the valid slot with the higher `seq` (Slot A on a tie).

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

## One image, either slot (copy-to-run, issue #97)

A slot is **staging only**. On a valid-slot boot the bootloader **copies** the
slot body to a single fixed SRAM run base and launches it there, via the shared
`ra8_dfu_launch`:

```
k_ra8_dfu_run_base = 0x22020000   (SRAM; clears the bootloader's low .bss, well
                                  below its stack at the top of SRAM)
```

So a payload is **linked once**, at the run base -- the *identical* `.bin` boots
from Slot A or Slot B, with no per-slot build and no "which slot?" footgun. This
sidesteps true position-independent code (ROPI/RWPI), which cannot relocate the
absolute function pointers this codebase's interface structs store in `.rodata`.
The header's `entry` records the run base; the bootloader copies to the trusted
`k_ra8_dfu_run_base` constant (not to `entry`), so a corrupted `entry` only fails
the `ra8_dfu_run_target_valid` cross-check and drops the boot to DFU.

Build the one image and stage it (the demo payload + builder live in
[`../dfu_copy_to_run`](../dfu_copy_to_run)):

```
# Link the payload ONCE at the run base (its linker_script.ld):
#   RUN (rwx) : ORIGIN = 0x22020000, LENGTH = ...
make <app>
arm-none-eabi-objcopy -O binary <app>.elf <app>.bin

# Then either let the bootloader's DFU device program it (operator path):
dfu-util -a 0 -D <app>.bin

# ...or stage the SAME .bin to either slot directly over J-Link (no host):
python3 stage_slot_image.py --payload <app>.bin --slot a --seq 1 --out slotA.hex
python3 stage_slot_image.py --payload <app>.bin --slot b --seq 2 --out slotB.hex
JLinkExe ... loadfile slotA.hex      # body @slot_base + header @slot_base+0x6FFE0
```

The same `<app>.bin` produces both slot images (identical body CRC).

## Validation (on-bench, EK-RA8D2, 2026-06-16)

The DFU **program/read-back** path is validated in both directions by the HIL
twins (Config A HS->FS, Config B FS->HS). The bootloader's own boot decision and
copy-to-run were validated locally over the J-Link OB:

- **No bootable slot -> DFU** (both slots erased). SCI8:
  ```
  ra8d2 dfu-bootloader (immutable, 128K @ 0x02000000)
  dfu-bootloader: no bootable slot -- entering DFU
  dfu-bootloader: DFU device up on USB-FS, awaiting DFU_DNLOAD...
  ```
- **One image, EITHER slot -> copy-to-run.** The *identical*
  [`dfu_copy_to_run`](../dfu_copy_to_run) `payload.bin` (body CRC `0x2869B9D7`,
  `entry=0x22020000`) was staged to Slot A (seq 10 > B) and, separately, to Slot B
  (seq 20 > A), then the bootloader was flashed and reset for each:
  ```
  RUN 1  dfu-bootloader: Slot A -> copy-to-run @0x22020000   PC=0x22020010
  RUN 2  dfu-bootloader: Slot B -> copy-to-run @0x22020000   PC=0x22020010
  ```
  In both runs J-Link confirmed the hand-off: the PC landed in the SRAM run
  window (`0x22020010`, reachable only by copying-to-run) and the payload's
  sentinel `mem32 0x22010000 = 9710C0DE` with an advancing heartbeat at
  `0x22010004`. Same `.bin`, both slots -- issue #97 closed.

- **DFU program + commit**, end to end with no external `dfu-util`: the board is
  its own DFU host over the self-loop ([`dfu_selftest_boot`](../dfu_selftest_boot))
  -- the HS host DFU_DNLOADs a payload + manifest and the FS DFU device programs
  **and commits** the slot header (`USB SELFTEST DFU-BOOT COMMIT PASS`). A commit
  stamps `entry=k_ra8_dfu_run_base`, i.e. the *exact* header format the
  copy-to-run proof above boots -- so the DFU-committed and J-Link-staged paths
  converge on the same bootable slot.

- **Unattended HIL gates** (`hil.conf`, `HIL_MODE=alive`). The dedicated
  [`dfu_copy_to_run`](../dfu_copy_to_run) app *always* copies-to-run, so its alive
  pass (PC in the SRAM run window) is an unambiguous copy-to-run proof. This
  bootloader's own alive gate (`scripts/hil/run_local.sh dfu_bootloader`) asserts
  PC in code, CycleCnt advancing, CFSR/HFSR clean, not in a fault spinner -- it
  passes whether the boot copies-to-run a valid slot or falls back to the DFU
  device. Local run: `PASS` with `PC=0x22020010` (it copied-to-run the valid slot
  into the SRAM run window, spinning cleanly with no fault).

## Recovery

The bootloader region is never erased by DFU, so a normal SWD re-flash of this
app (`scripts/dev/flash.sh` / `make flash`) always recovers the board. A bad slot is
self-healing (CRC fails -> the older valid slot boots).
