# ecc_monitor_demo

SRAM **ECC** (SECDED) bring-up + hardware-error-status monitor for the
bare EK-RA8D2 EVM. Exercises the `ra8_sram` ECC path (#130).

## What it does

Brings up SCI8 + LEDs, then enables full ECC ("with-check") on a **spare**
SRAM bank and proves it works:

1. `ra8_sram_init` -- enables with-check ECC + 1-bit-error latch + the
   `zero_init` pass on **bank 2** (`0x2210_0000`). The other banks stay
   ECC-disabled.
2. Round-trips a deterministic pattern through an ECC-protected buffer at
   the bank base, byte-exact (`g_ecc_rw_ok`).
3. Reads `SRAMESR` (the ECC error-status register) and reports it.

Each second: `ecc: sram2 ecc=on rw=ok ok=Y`. LED1 toggles while healthy;
LED2 on a round-trip fault. `g_ecc_ok` / `g_ecc_rw_ok` / `g_ecc_esr` /
`g_ecc_1bit` / `g_ecc_2bit` / `g_ecc_heartbeat` mirror the result for
headless probing.

No external hardware required.

### Why bank 2 (safety)

ECC with-check requires the bank's ECC codes to be initialised first
(`zero_init`), or reading a never-written word raises a spurious **2-bit
error -> NMI**. The zero-init pass rewrites the whole bank, so it must
target memory the program does not use. This app's linker keeps `.data` /
`.bss` / stack in the first 1 MiB (banks 0-1); banks 2-3 are unused, so
ECC + zero-init on bank 2 cannot disturb the running program. `on_error`
stays NMI but no error is injected, so the NMI never fires here.

## Why this is in hw_pending

`tools/ra8_emulator` shadows the SRAM ECC control window (`SRAMCRn` /
`SRAMESR`) so the bring-up + round-trip run and report `ok=Y` -- but
ra8_emulator does **not** model ECC, so the actual error-detection path (a
real 1-bit / 2-bit flip setting `SRAMESR`) cannot be exercised on the
emulator. The headless verdict therefore gates only on the deterministic
round-trip; the latched error masks (`g_ecc_1bit` / `g_ecc_2bit`) are
reported for on-silicon probing. ECC error detection can only be confirmed
on the bench, so the app stays in `hw_pending/`.

## Registers (HUM R01UH1065EJ0130 Rev.1.30, Ch 58 "SRAM")

- Per-bank ECC mode in `SRAMCRn` (HUM Ch 58.2.x p 3527+); the ECC region
  size is `SRAMECCRGNn`; the deterministic zero-init pass lays down valid
  ECC (HUM Ch 58.3.2).
- ECC error status read from `SRAMESR` (HUM Ch 58.2.12), decoded by
  `ra8_sram_get_status` into per-bank 1-bit / 2-bit masks.

## On-silicon bench plan

1. `make ecc_monitor_demo`, then flash the EK-RA8D2.
2. Confirm the bring-up: `ecc: sram2 ecc=on rw=ok ok=Y` once a second
   (`g_ecc_ok == 1`, `g_ecc_1bit == 0`, `g_ecc_2bit == 0`).
3. **Error detection (the real acceptance, needs an error source):**
   inject a 1-bit error -- e.g. via the ECC test-mode write path (writing
   a deliberately wrong ECC code), then read the word back and confirm
   `g_ecc_1bit` latches the bank bit and `SRAMESR` reports it (corrected),
   and that a 2-bit error drives the NMI handler.
4. Once error detection is confirmed, move the app to `hw_validated/hil/`.

Build / flash:

```
make ecc_monitor_demo
make -C examples/ek_ra8d2/hw_pending/ecc_monitor_demo flash
```
