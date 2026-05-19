# ra_bootloader

Minimal A/B bank-switch bootloader stub for the RA8D2 OTA pipeline.
Runs first out of reset, reads a single bank-config byte from a fixed
MRAM location, validates the candidate vector table, and hands control
to the active application bank.

## What it does

The 1 MiB MRAM is split into two 512 KiB banks at `0x02000000` (bank A)
and `0x02080000` (bank B). The bank-config byte at `0x02003F00` selects
which bank to enter:

| Selector byte    | Bank chosen |
|------------------|-------------|
| `0x00` / `0xFF`  | A (default / erased) |
| `0xA5`           | B           |
| anything else    | A (default-deny fallback) |

The hand-off mirrors the Cortex-M85 reset sequence:

1. `cpsid i` -- mask IRQs.
2. Read `app_msp = *(uint32_t*)bank_base`.
3. Read `app_reset = *(uint32_t*)(bank_base + 4)`.
4. Re-point `SCB.VTOR` at `bank_base`.
5. `msr msp, app_msp` then `bx app_reset`.

A sanity check rejects banks whose MSP is outside SRAM
(`0x22000000`-`0x22200000`) or whose Reset_Handler is outside MRAM
(`0x02000000`-`0x02100000`); fully-erased banks therefore fail closed
and the bootloader falls back to the other slot. If both banks are
unbootable the bootloader spins in `wfi` so a debugger can attach.

This is a deliberately minimal stub -- a production OTA stack would
add CRC verification and a roll-back counter on top of the selector
byte. It does not depend on the HAL.

## Build + flash

From the repo root:

```sh
make ra_bootloader
bash scripts/flash.sh build/ra_bootloader/ra_bootloader.hex
```

## Notes

- No BSP / HAL dependency -- just `<stdint.h>` and inline register
  accessors, so the bootloader stays small enough to fit in the first
  16 KiB of bank A (application vectors begin at `0x02004000`).
- The handoff is `__attribute__((noreturn))`; control never returns
  to this image.

Validated 2026-05-02 against HUM (R01UH1065EJ0130) Ch "MRAM" + Ch
"System Control Block (SCB.VTOR)", and the ARMv8-M Architecture
Reference Manual reset / vector-fetch sequence.

## HIL plan

**HIL-able after firmware work -- A/B state machine is currently a
stub.** The bootloader needs a second flashable image installed in
the alternate MRAM bank before its bank-switch path can be
exercised end-to-end; that scaffolding does not exist yet (no
companion `ra_app_a` / `ra_app_b` build artifacts, no MRAM-write
journaling for the bank-config byte).

Once the alternate-image scaffolding lands, a candidate HIL flow:
- Flash the bootloader at the MRAM base.
- Use `hil_jlink_memprobe` (write-mode, if added) to set the
  bank-config byte to 'B'.
- Reset and confirm via `uart_scrape` that a boot banner from the
  alternate image appears (e.g. `app_b: hello`).
- Flip the byte back, reset, assert the 'A' banner.

That requires bootloader firmware work first; until then the app is
build-only.

Stays in `hw_pending/` -- bootloader state machine is documented as
a stub.
