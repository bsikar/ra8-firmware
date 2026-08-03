<!-- Copyright (c) 2026 Brighton Sikarskie / SPDX-License-Identifier: MIT -->
# mpu_boot_map_hal -- MPU boot map via the `ra8_mpu` HAL (#576)

Single-core (Cortex-M85) demonstrator that brings the boot MPU
memory-attribute map up **through the `ra8_mpu` HAL**
(`ra8_mpu_apply_boot_map()`) instead of the hand-rolled MAIR / RBAR / RLAR /
CTRL register pokes that the shared board boot uses.

It is the HAL-path twin of
[`cache_mpu_hil`](../../hw_validated/hil/cache_mpu_hil) (the raw-poke
reference). Both apps enable the L1 caches and the same 5-region MPU; the only
difference is *how* the map is installed. Diff the two boot files to see it:

```sh
diff libs/ra8_board_ek_ra8d2/boot/system_init.c \
     examples/ek_ra8d2/hw_pending/mpu_boot_map_hal/system_init.c
```

Every core-bring-up step is identical; the ~90 lines of raw
`internal_mpu_set_region()` / `internal_mpu_init()` collapse to a single
`ra8_mpu_apply_boot_map()` call.

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
`ra8_mpu_set_region()` rejects it. `ra8_mpu_apply_boot_map()` encodes the map
by base+limit directly, which the Armv8-M PMSAv8 RBAR/RLAR pair supports
natively. That is the "missing primitive" this app exercises.

## Self-test

`main()` runs three independent checks and prints a one-shot banner:

1. **MPU enabled via the HAL** -- `ra8_mpu_is_enabled()` (no direct register
   poke) confirms `SystemInit()` ran `ra8_mpu_apply_boot_map()`.
2. **Canonical boot map** -- `ra8_mpu_boot_map()` exposes the 5-region table,
   and region 4 is the 640 KiB non-cacheable shared bank.
3. **Device-nGnRE MMIO** -- a live `SCKDIVCR` readback proves region 3 is
   mapped Device and reachable.

On success it emits over the J-Link OB VCOM console (SCI8, PD02/PD03 @ 115200
8N1):

```
mpu_boot_map_hal: mpu-via-hal PASS
```

Any failure prints a distinct `... FAIL` line (never containing `PASS`) and the
core parks in WFI.

## Build / flash

```sh
make            # cross-compile build/mpu_boot_map_hal.elf / .hex / .bin
make flash      # load via scripts/dev/flash.sh
make size       # arm-none-eabi-size on the ELF
```

## Status

`hw_pending` -- written and host/emulator-checked, not yet bench-validated on
silicon.
